import Foundation
@main struct Tests {
 static func main() {
  func parse(_ exec: String, _ extra: String = "") -> [String: Any]? { DesktopEntry.configuration("[Desktop Entry]\nType=Application\nName=Real Qt App\nExec=\(exec)\n\(extra)",path:"/usr/share/applications/qt.desktop") }
  precondition(parse("qt-test %U")?["arguments"] as? [String] == [])
  precondition(parse("qt-test --title %c %k")?["arguments"] as? [String] == ["--title","Real Qt App","/usr/share/applications/qt.desktop"])
  precondition(parse("qt-test %i", "Icon=real\n")?["arguments"] as? [String] == ["--icon","real"])
  precondition(parse("\"/opt/Qt App/test\" \"one two\" 100%%")?["arguments"] as? [String] == ["one two","100%"])
  for invalid in ["qt %Z", "qt | echo", "qt 'bad'", "qt \"unclosed", "qt \"%U\"", "a=b", "qt foo%F", "qt $HOME"] { precondition(parse(invalid) == nil, invalid) }
  let firefox = parse("/bin/sh \"/home/mobile/Downloads/.linuxrunner/gui/firefox-launch.sh\" /usr/lib/firefox-esr/firefox-esr %u", "DBusActivatable=false\n")!
  precondition(firefox["executable"] as? String == "/bin/sh")
  precondition(firefox["arguments"] as? [String] == ["/home/mobile/Downloads/.linuxrunner/gui/firefox-launch.sh", "/usr/lib/firefox-esr/firefox-esr"])
  precondition(firefox["dbusActivatable"] as? Bool == false)
  precondition(parse("qt", "Hidden=true\n") == nil)
  precondition(parse("qt", "NoDisplay=true\n") == nil)
  precondition(parse("qt", "Terminal=true\n")?["terminal"] as? Bool == true)
  precondition(parse("qt", "Path=relative\n") == nil)
  precondition(parse("qt %U %f") == nil)
  let metadata = parse("qt", "GenericName=Editor\nComment=Edit\\sfiles\nIcon=editor\nCategories=Utility;TextEditor;\n")!
  precondition(metadata["genericName"] as? String == "Editor" && metadata["comment"] as? String == "Edit files")
  precondition(metadata["categories"] as? [String] == ["Utility", "TextEditor"])
  for value in ["firefox-esr", "libqt6widgets6:arm64", "g++", "qt6-qtbase", "a_b"] { precondition(PackageOperation.validName(value)) }
  for value in ["-y", "a;id", "$(id)", "a b", "../../x", "", "x\n"] { precondition(!PackageOperation.validName(value)) }
  precondition((try? PackageOperation.command("install", value: "x;id")) == nil)
  precondition((try? PackageOperation.command("upgrade")) == nil)
  precondition((try? PackageOperation.command("upgrade", value: "-y")) == nil)
  precondition(try! PackageOperation.command("upgrade", value: "hello").suffix(2) == ["upgrade", "hello"])
  precondition(try! PackageOperation.command("install", value: "pcmanfm") == ["/bin/sh", "/home/mobile/Downloads/.linuxrunner/gui/package-manager.sh", "install", "pcmanfm"])
  precondition(LinuxPackageRecord.parse("xterm\t398-1\tarm64\tii \nnot a package\t1\tarm64\tii ").count == 1)
  print("PASS: desktop literal argv, field-code expansion, quoting, no shell evaluation, hidden/terminal/path handling")
 }
}
