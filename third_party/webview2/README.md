Microsoft.Web.WebView2 SDK 1.0.4258.31

Vendored from the existing local TitanLeiaBridge SDK cache, preserving the
Microsoft NuGet package headers, x64 WebView2LoaderStatic.lib and LICENSE.txt.
Package identity was checked against Microsoft.Web.WebView2.nuspec.
Only the static loader is shipped in the application; the Evergreen WebView2
Runtime must already be installed on each computer.

The application embeds its local HTML/CSS/JavaScript as resource 201. It does
not download a UI or require a web server. User data/cache is stored below
%LOCALAPPDATA%\LAN3DShare. External navigation and downloads are disabled.
