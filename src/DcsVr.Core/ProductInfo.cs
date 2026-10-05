using System.Reflection;

namespace DcsVr.Core;

/// <summary>Product version from Directory.Build.props, so reports, packages and scripts share one source.</summary>
public static class ProductInfo
{
    public static string Version { get; } = typeof(ProductInfo).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion.Split('+')[0]
        ?? throw new InvalidOperationException("The assembly has no product version.");
}
