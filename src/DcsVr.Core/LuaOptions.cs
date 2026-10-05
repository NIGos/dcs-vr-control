using System.Globalization;
using System.Text;

namespace DcsVr.Core;

/// <summary>Parses data-only Lua tables. It never executes Lua or rewrites unrelated entries.</summary>
public sealed class LuaOptions
{
    private sealed record Token(string Kind, string Text, int Start, int End);
    private sealed record Node(int Start, int End, string Raw, Dictionary<string, Node>? Fields, int ClosingBrace, int FieldStart = -1, int FieldEnd = -1);
    private readonly string _text;
    private readonly Node _root;

    public LuaOptions(string text)
    {
        _text = text;
        var tokens = Tokenize(text);
        var position = 0;
        if (tokens.Count < 3 || tokens[0].Text != "options" || tokens[1].Text != "=")
            throw new InvalidDataException("options.lua must contain an 'options' data table.");
        position = 2;
        _root = ParseNode(tokens, ref position, text, 0);
        if (_root.Fields is null || position != tokens.Count)
            throw new InvalidDataException("Unsupported Lua syntax: the file will not be executed or modified.");
    }

    public string? Get(params string[] path) => Find(path)?.Raw;

    public string Set(IReadOnlyList<string> path, object value) => SetLiteral(path, Literal(value));

    internal string SetLiteral(IReadOnlyList<string> path, string literal)
    {
        if (path.Count == 0) throw new ArgumentException("The Lua path is empty.", nameof(path));
        var verification = new LuaOptions("options={value=" + literal + "}");
        if (verification.Find(["value"])?.Fields is not null) throw new InvalidDataException("Expected a scalar Lua literal.");
        var existing = Find(path);
        if (existing is not null)
        {
            if (existing.Fields is not null) throw new InvalidDataException("A table cannot be replaced with a scalar value.");
            return _text[..existing.Start] + literal + _text[existing.End..];
        }
        var parent = Find(path.Take(path.Count - 1).ToArray())
            ?? throw new InvalidDataException("The target Lua table is missing.");
        if (parent.Fields is null) throw new InvalidDataException("The Lua container is not a table.");
        var tableTokens = Tokenize(_text[parent.Start..parent.ClosingBrace]);
        var needsComma = tableTokens.Count > 1 && tableTokens[^1].Text is not ("," or ";" or "{");
        var newline = _text.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
        var insertion = (needsComma ? "," : "") + newline + "\t\t[" + Literal(path[^1]) + "] = " + literal + "," + newline;
        return _text[..parent.ClosingBrace] + insertion + _text[parent.ClosingBrace..];
    }

    internal string Remove(IReadOnlyList<string> path)
    {
        var entry = Find(path);
        if (entry is null) return _text;
        if (entry.Fields is not null || entry.FieldStart < 0 || entry.FieldEnd < entry.FieldStart) throw new InvalidDataException("Cannot remove this Lua field.");
        var result = _text[..entry.FieldStart] + _text[entry.FieldEnd..];
        _ = new LuaOptions(result); return result;
    }
    public static string? NormalizeLiteral(string? raw)
    {
        if (raw is null) return null;
        var tokens = Tokenize(raw);
        if (tokens.Count != 1) throw new InvalidDataException("Expected one Lua scalar literal.");
        // Numbers compare by value, so DCS rewriting 90 as 90.0 or 0x5A is not reported as a conflict.
        return tokens[0].Kind == "number" ? "number:" + ParseNumber(tokens[0].Text).ToString("R", CultureInfo.InvariantCulture) : tokens[0].Kind + ":" + tokens[0].Text;
    }

    public static string Patch(string text, IReadOnlyDictionary<string, object> changes)
    {
        foreach (var (path, value) in changes) text = new LuaOptions(text).Set(path.Split('.'), value);
        _ = new LuaOptions(text);
        return text;
    }

    private Node? Find(IReadOnlyList<string> path)
    {
        var node = _root;
        foreach (var key in path)
        {
            if (node.Fields is null || !node.Fields.TryGetValue(key, out node)) return null;
        }
        return node;
    }

    private static string Literal(object value) => value switch
    {
        bool b => b ? "true" : "false",
        string s => "\"" + s.Replace("\\", "\\\\", StringComparison.Ordinal).Replace("\"", "\\\"", StringComparison.Ordinal)
            .Replace("\r", "\\r", StringComparison.Ordinal).Replace("\n", "\\n", StringComparison.Ordinal).Replace("\t", "\\t", StringComparison.Ordinal) + "\"",
        int n => n.ToString(CultureInfo.InvariantCulture),
        double d when double.IsFinite(d) => d.ToString("G17", CultureInfo.InvariantCulture),
        _ => throw new ArgumentException("The Lua value must be a string, boolean, or finite number.")
    };

    private static Node ParseNode(List<Token> tokens, ref int pos, string source, int depth)
    {
        if (depth > 64 || pos >= tokens.Count) throw new InvalidDataException("Lua table is too deep or incomplete.");
        var start = tokens[pos];
        if (start.Text == "{")
        {
            pos++;
            var fields = new Dictionary<string, Node>(StringComparer.Ordinal);
            while (pos < tokens.Count && tokens[pos].Text != "}")
            {
                var fieldStart = tokens[pos].Start;
                string? key = null;
                if (tokens[pos].Text == "[")
                {
                    pos++;
                    if (pos >= tokens.Count || tokens[pos].Kind is not ("string" or "number")) throw new InvalidDataException("Unsupported Lua key.");
                    if (tokens[pos].Kind == "string") key = tokens[pos].Text;
                    pos++;
                    Expect(tokens, ref pos, "]"); Expect(tokens, ref pos, "=");
                }
                else if (tokens[pos].Kind == "identifier" && pos + 1 < tokens.Count && tokens[pos + 1].Text == "=")
                { key = tokens[pos++].Text; pos++; }
                var child = ParseNode(tokens, ref pos, source, depth + 1);
                if (pos < tokens.Count && tokens[pos].Text is "," or ";") pos++;
                else if (pos >= tokens.Count || tokens[pos].Text != "}") throw new InvalidDataException("Missing Lua separator.");
                child = child with { FieldStart = fieldStart, FieldEnd = tokens[pos - 1].End };
                if (key is not null && !fields.TryAdd(key, child)) throw new InvalidDataException($"Duplicate Lua key: {key}.");
            }
            if (pos >= tokens.Count) throw new InvalidDataException("Unterminated Lua table.");
            var close = tokens[pos++];
            return new(start.Start, close.End, source[start.Start..close.End], fields, close.Start);
        }
        if (start.Kind is not ("string" or "number") && start.Text is not ("true" or "false" or "nil"))
            throw new InvalidDataException("Executable Lua expressions are unsupported.");
        pos++;
        return new(start.Start, start.End, source[start.Start..start.End], null, -1);
    }

    private static void Expect(List<Token> tokens, ref int pos, string symbol)
    {
        if (pos >= tokens.Count || tokens[pos++].Text != symbol) throw new InvalidDataException($"Expected '{symbol}' in Lua file.");
    }

    private static List<Token> Tokenize(string text)
    {
        if (text.Length > 4 * 1024 * 1024) throw new InvalidDataException("options.lua exceeds 4 MiB.");
        var tokens = new List<Token>();
        var i = text.Length > 0 && text[0] == '\uFEFF' ? 1 : 0;
        while (i < text.Length)
        {
            if (char.IsWhiteSpace(text[i])) { i++; continue; }
            if (text[i] == '-' && i + 1 < text.Length && text[i + 1] == '-')
            {
                i += 2;
                if (TryLong(text, i, out var commentEnd, out _)) i = commentEnd;
                else { while (i < text.Length && text[i] != '\n') i++; }
                continue;
            }
            var start = i;
            if (TryLong(text, i, out var longEnd, out var longValue))
            { tokens.Add(new("string", longValue, i, longEnd)); i = longEnd; continue; }
            if (text[i] is '\'' or '"')
            {
                var quote = text[i++]; var value = new StringBuilder(); var closed = false;
                while (i < text.Length)
                {
                    var c = text[i++];
                    if (c == quote) { closed = true; break; }
                    if (c == '\\')
                    {
                        if (i == text.Length) throw new InvalidDataException("Incomplete Lua escape.");
                        i = AppendEscape(text, i, value); continue;
                    }
                    value.Append(c);
                }
                if (!closed) throw new InvalidDataException("Unterminated Lua string.");
                tokens.Add(new("string", value.ToString(), start, i)); continue;
            }
            var digitAt = text[i] == '-' ? i + 1 : i;
            if (digitAt < text.Length && (char.IsAsciiDigit(text[digitAt]) || (text[digitAt] == '.' && digitAt + 1 < text.Length && char.IsAsciiDigit(text[digitAt + 1]))))
            {
                i = digitAt;
                if (text[i] == '0' && i + 1 < text.Length && text[i + 1] is 'x' or 'X')
                { i += 2; while (i < text.Length && char.IsAsciiHexDigit(text[i])) i++; }
                else
                {
                    while (i < text.Length && (char.IsAsciiDigit(text[i]) || text[i] is '.' or 'e' or 'E'
                        || (text[i] is '+' or '-' && text[i - 1] is 'e' or 'E'))) i++;
                }
                var number = text[start..i];
                _ = ParseNumber(number);
                tokens.Add(new("number", number, start, i)); continue;
            }
            if (char.IsAsciiLetter(text[i]) || text[i] == '_')
            {
                i++; while (i < text.Length && (char.IsAsciiLetterOrDigit(text[i]) || text[i] == '_')) i++;
                tokens.Add(new("identifier", text[start..i], start, i)); continue;
            }
            if ("{}[]=,;".Contains(text[i])) { tokens.Add(new("symbol", text[i].ToString(), i, i + 1)); i++; continue; }
            throw new InvalidDataException($"Unsupported Lua character at offset {i}.");
        }
        return tokens;
    }

    private static double ParseNumber(string number)
    {
        var negative = number.StartsWith('-'); var body = negative ? number[1..] : number;
        double value;
        if (body.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            if (body.Length == 2 || body.Length > 18 || !ulong.TryParse(body[2..], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var hex))
                throw new InvalidDataException("Invalid Lua number.");
            value = hex;
        }
        else if (!double.TryParse(body, NumberStyles.AllowDecimalPoint | NumberStyles.AllowExponent, CultureInfo.InvariantCulture, out value) || !double.IsFinite(value))
            throw new InvalidDataException("Invalid Lua number.");
        return negative ? -value : value;
    }

    /// <summary>Decodes one Lua escape after the backslash at <paramref name="i"/>; returns the next index.</summary>
    private static int AppendEscape(string text, int i, StringBuilder value)
    {
        var c = text[i++];
        switch (c)
        {
            case 'n': value.Append('\n'); return i;
            case 'r': value.Append('\r'); return i;
            case 't': value.Append('\t'); return i;
            case 'a': value.Append('\a'); return i;
            case 'b': value.Append('\b'); return i;
            case 'f': value.Append('\f'); return i;
            case 'v': value.Append('\v'); return i;
            case '\r': if (i < text.Length && text[i] == '\n') i++; value.Append('\n'); return i;
            case '\n': if (i < text.Length && text[i] == '\r') i++; value.Append('\n'); return i;
            case >= '0' and <= '9':
                var end = i - 1; while (end < text.Length && end < i + 2 && char.IsAsciiDigit(text[end])) end++;
                var code = int.Parse(text[(i - 1)..end], CultureInfo.InvariantCulture);
                if (code > 255) throw new InvalidDataException("Invalid Lua decimal escape.");
                value.Append((char)code); return end;
            // \, ", ' and, as in DCS's Lua 5.1, any other character: \x and \z are Lua 5.2 escapes.
            default: value.Append(c); return i;
        }
    }

    private static bool TryLong(string text, int start, out int end, out string value)
    {
        end = start; value = "";
        if (start >= text.Length || text[start] != '[') return false;
        var i = start + 1;
        while (i < text.Length && text[i] == '=') i++;
        if (i >= text.Length || text[i] != '[') return false;
        var marker = "]" + new string('=', i - start - 1) + "]";
        var close = text.IndexOf(marker, i + 1, StringComparison.Ordinal);
        if (close < 0) throw new InvalidDataException("Unterminated long Lua string or comment.");
        value = text[(i + 1)..close]; end = close + marker.Length; return true;
    }
}
