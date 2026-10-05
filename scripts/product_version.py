"""Product version from Directory.Build.props, the single source of truth."""
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent


def product_version() -> str:
    group = ET.parse(ROOT / 'Directory.Build.props').getroot().find('PropertyGroup')
    prefix = group.findtext('VersionPrefix')
    if not prefix:
        raise RuntimeError('Directory.Build.props has no VersionPrefix.')
    suffix = group.findtext('VersionSuffix')
    return f'{prefix}-{suffix}' if suffix else prefix
