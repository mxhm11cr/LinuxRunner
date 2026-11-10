import SwiftUI

/// Bluetooth LE serial links for Arduino/ESP32-class boards, exposed to Linux
/// as /dev/ttyBLE0-3. iOS offers apps no generic USB host access, so this and
/// Wi-Fi are the connections that genuinely work; see EmbeddedCompatibilityView.
struct BluetoothSerialView: View {
    @State private var state = "unknown"
    @State private var discovered: [[String: Any]] = []
    @State private var links: [[String: Any]] = []
    @State private var scanning = false
    @State private var showAll = false
    private let bridge = LRBLESerial.shared()

    var body: some View {
        List {
            Section {
                HStack(spacing: 12) {
                    RoundedRectangle(cornerRadius: 10, style: .continuous).fill(LinearGradient(colors: [.cyan, .blue], startPoint: .topLeading, endPoint: .bottomTrailing)).frame(width: 40, height: 40)
                        .overlay(Image(systemName: "dot.radiowaves.left.and.right").foregroundColor(.white))
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Bluetooth Serial").font(.headline)
                        Text(stateText).font(.caption).foregroundColor(state == "poweredOn" ? .secondary : .orange)
                    }
                    Spacer()
                    Button(scanning ? "Stop" : "Scan") { scanning ? bridge.stopScan() : bridge.startScan() }.buttonStyle(.bordered)
                }
            } footer: {
                Text("For boards running Bluetooth LE UART firmware: ESP32 with the Nordic UART Service, Arduino boards with HM-10/CC254x modules, Microchip RN4870/BM70. Each connection becomes a Linux serial port.")
            }
            if !links.isEmpty {
                Section("Linux Serial Ports") {
                    ForEach(links.indices, id: \.self) { i in
                        let link = links[i]
                        VStack(alignment: .leading, spacing: 4) {
                            HStack {
                                Circle().fill(color(link["state"] as? String)).frame(width: 8, height: 8)
                                Text(link["path"] as? String ?? "").font(.system(.body, design: .monospaced).weight(.semibold))
                                Spacer()
                                Text((link["state"] as? String ?? "").capitalized).font(.caption).foregroundColor(.secondary)
                            }
                            Text("\(link["name"] as? String ?? "") · \(link["profile"] as? String ?? "")").font(.caption)
                            Text("Received \(bytes(link["rx"])) · Sent \(bytes(link["tx"]))").font(.caption2).foregroundColor(.secondary)
                            if let error = link["error"] as? String, !error.isEmpty { Text(error).font(.caption2).foregroundColor(.orange) }
                        }
                        .swipeActions { Button("Disconnect", role: .destructive) { bridge.disconnectSlot(link["slot"] as? Int ?? -1) } }
                        .contextMenu {
                            if link["state"] as? String != "connected" { Button("Reconnect") { _ = bridge.connect(link["id"] as? String ?? "") } }
                            Button("Disconnect", role: .destructive) { bridge.disconnectSlot(link["slot"] as? Int ?? -1) }
                        }
                    }
                    Text("In the Linux terminal: picocom -b 115200 /dev/ttyBLE0, or pyserial serial.Serial(\"/dev/ttyBLE0\"). The baud rate is accepted but BLE ignores it; the board's firmware sets its own UART speed.").font(.caption).foregroundColor(.secondary)
                }
            }
            Section {
                let rows = discovered.filter { showAll || ($0["serial"] as? Bool ?? false) }
                if rows.isEmpty { Text(scanning ? "Looking for boards…" : "Tap Scan to look for nearby boards.").foregroundColor(.secondary) }
                ForEach(rows.indices, id: \.self) { i in
                    let device = rows[i]
                    Button { _ = bridge.connect(device["id"] as? String ?? "") } label: {
                        HStack {
                            Image(systemName: (device["serial"] as? Bool ?? false) ? "cpu" : "antenna.radiowaves.left.and.right").foregroundColor(.accentColor).frame(width: 24)
                            VStack(alignment: .leading) {
                                Text(device["name"] as? String ?? "").foregroundColor(.primary)
                                Text((device["profile"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? "Serial service not advertised").font(.caption).foregroundColor(.secondary)
                            }
                            Spacer()
                            Text("\((device["rssi"] as? NSNumber)?.intValue ?? 0) dBm").font(.caption2).foregroundColor(.secondary)
                        }
                    }
                }
                Toggle("Show devices without an advertised serial service", isOn: $showAll).font(.footnote)
            } header: { Text("Nearby") } footer: {
                Text("Some firmware does not advertise its UART service; connecting checks the device's services and reports when none is supported.")
            }
            Section { NavigationLink(destination: EmbeddedCompatibilityView()) { Label("What Works for Embedded Development", systemImage: "checklist") } }
        }
        .navigationTitle("Bluetooth Serial")
        .onAppear { refresh() }
        .onDisappear { bridge.stopScan() }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("LRBLESerialChanged"))) { _ in refresh() }
    }
    private var stateText: String {
        switch state {
        case "poweredOn": return scanning ? "Scanning" : "Ready"
        case "poweredOff": return "Bluetooth is off. Turn it on in Control Center."
        case "unauthorized": return "Bluetooth access is off for LinuxRunner. Allow it in iOS Settings › Privacy › Bluetooth."
        case "unsupported": return "This device does not support Bluetooth LE."
        default: return "Tap Scan to start Bluetooth."
        }
    }
    private func refresh() { state = bridge.bluetoothState; scanning = bridge.scanning; discovered = bridge.discovered; links = bridge.links }
    private func color(_ s: String?) -> Color { s == "connected" ? .green : (s == "connecting" || s == "discovering") ? .orange : .red }
    private func bytes(_ v: Any?) -> String { ByteCountFormatter.string(fromByteCount: (v as? NSNumber)?.int64Value ?? 0, countStyle: .binary) }
}

/// Honest compatibility matrix for Arduino/ESP32-style work on iPhone/iPad.
struct EmbeddedCompatibilityView: View {
    private struct Entry: Identifiable { let id = UUID(); let name, status, detail: String; let level: Int } // 2 works, 1 partial, 0 not possible
    private let entries: [Entry] = [
        Entry(name: "Bluetooth LE serial (ESP32 Nordic UART, HM-10, RN4870)", status: "Supported", detail: "Appears as /dev/ttyBLE0-3: terminal programs, pyserial, arduino-cli monitor. Throughput is BLE-limited (typically a few KB/s). Tested on the Mac with the Linux tty layer; tested with a physical board: not yet.", level: 2),
        Entry(name: "Wi-Fi: OTA upload (ArduinoOTA, espota.py), telnet/RFC2217 serial bridges, MQTT, HTTP", status: "Supported", detail: "Uses Linux networking directly. pyserial accepts socket:// and rfc2217:// URLs; PlatformIO and arduino-cli support OTA upload to network ports.", level: 2),
        Entry(name: "Compiling sketches (arduino-cli, PlatformIO, esp-idf)", status: "Partial", detail: "The ARM64 Linux toolchains run in LinuxRunner's interpreter; builds are slow. Install from the App Store (arduino-cli, PlatformIO Core, esptool).", level: 1),
        Entry(name: "USB serial through a USB-MIDI adapter (Raspberry Pi Pico, ESP32-S2/S3, Leonardo/Micro, Teensy)", status: "Supported", detail: "Boards with native USB run LinuxRunner's serial-over-MIDI firmware once (flashed from a computer) and appear as /dev/ttyMIDI0-3, with baud rate and DTR/RTS passed to the adapter. iOS supports USB MIDI for every app without special permission. App side tested on the Mac; with a physical adapter: not yet.", level: 2),
        Entry(name: "Flashing a board (esptool, avrdude) through the USB-MIDI adapter", status: "Partial", detail: "The adapter mirrors DTR/RTS on two GPIOs wired to the target's reset and boot pins (EN/IO0 on ESP32, RESET on AVR), the auto-reset circuit USB serial boards use. Not yet tested with hardware.", level: 1),
        Entry(name: "USB serial chips plugged in directly (CH340, CP210x, FTDI, native USB CDC-ACM)", status: "Not possible", detail: "iOS gives ordinary apps no USB host access to these chips. DriverKit USB drivers need entitlements Apple grants case by case and only run on iPad with Apple silicon. Use a USB-MIDI adapter between the phone and the board.", level: 0),
        Entry(name: "Classic Bluetooth serial (HC-05/HC-06, ESP32 BluetoothSerial SPP)", status: "Not possible", detail: "iOS exposes classic Bluetooth SPP only to MFi-certified accessories.", level: 0),
        Entry(name: "MFi accessories (External Accessory)", status: "Not available", detail: "Requires accessories certified for a protocol named by the app; development boards are not.", level: 0),
    ]
    var body: some View {
        List {
            ForEach(entries) { e in
                VStack(alignment: .leading, spacing: 4) {
                    HStack(alignment: .top) {
                        Image(systemName: e.level == 2 ? "checkmark.circle.fill" : e.level == 1 ? "circle.lefthalf.filled" : "xmark.circle.fill")
                            .foregroundColor(e.level == 2 ? .green : e.level == 1 ? .orange : .red)
                        Text(e.name).font(.subheadline.weight(.semibold))
                        Spacer()
                        Text(e.status).font(.caption).foregroundColor(.secondary)
                    }
                    Text(e.detail).font(.caption).foregroundColor(.secondary)
                }.padding(.vertical, 2)
            }
        }.navigationTitle("Embedded Development")
    }
}

/// USB serial through a USB-MIDI adapter (CoreMIDI, no entitlement): boards or
/// adapters running LinuxRunner's serial-over-MIDI firmware become /dev/ttyMIDI0-3.
struct USBSerialView: View {
    @State private var links: [[String: Any]] = []
    @State private var devices: [[String: Any]] = []
    private let bridge = LRMIDISerial.shared()
    var body: some View {
        List {
            Section {
                HStack(spacing: 12) {
                    RoundedRectangle(cornerRadius: 10, style: .continuous).fill(LinearGradient(colors: [.orange, .red], startPoint: .topLeading, endPoint: .bottomTrailing)).frame(width: 40, height: 40)
                        .overlay(Image(systemName: "cable.connector").foregroundColor(.white))
                    VStack(alignment: .leading, spacing: 2) {
                        Text("USB Serial Adapter").font(.headline)
                        Text(links.isEmpty ? "No adapter connected" : "\(links.count) connected").font(.caption).foregroundColor(.secondary)
                    }
                    Spacer()
                    Button("Rescan") { bridge.rescan() }.buttonStyle(.bordered)
                }
            } footer: {
                Text("iPhone and iPad cannot open the USB serial chips on Arduino or ESP32 boards. A board with native USB (Raspberry Pi Pico, ESP32-S2/S3, Arduino Leonardo/Micro, Teensy) running LinuxRunner's serial-over-MIDI firmware connects through iOS's built-in USB MIDI support instead, as its own serial port or as an adapter for another board, including its reset lines for flashing.")
            }
            Section("Linux Serial Ports") {
                if links.isEmpty { Text("Connect an adapter with the USB cable (USB-C, or Lightning with Apple's camera adapter).").foregroundColor(.secondary) }
                ForEach(links.indices, id: \.self) { i in
                    let link = links[i]
                    VStack(alignment: .leading, spacing: 4) {
                        HStack { Circle().fill(Color.green).frame(width: 8, height: 8)
                            Text(link["path"] as? String ?? "").font(.system(.body, design: .monospaced).weight(.semibold)); Spacer()
                            Text("\((link["baud"] as? NSNumber)?.intValue ?? 0) baud").font(.caption).foregroundColor(.secondary) }
                        Text("\(link["name"] as? String ?? "") · firmware v\((link["version"] as? NSNumber)?.intValue ?? 0)").font(.caption)
                        Text("Received \(ByteCountFormatter.string(fromByteCount: (link["rx"] as? NSNumber)?.int64Value ?? 0, countStyle: .binary)) · Sent \(ByteCountFormatter.string(fromByteCount: (link["tx"] as? NSNumber)?.int64Value ?? 0, countStyle: .binary))").font(.caption2).foregroundColor(.secondary)
                    }
                }
                if !links.isEmpty { Text("picocom -b 115200 /dev/ttyMIDI0 · esptool.py --port /dev/ttyMIDI0 flash_id · arduino-cli monitor -p /dev/ttyMIDI0").font(.caption.monospaced()).foregroundColor(.secondary) }
            }
            Section("USB MIDI Devices iOS Sees") {
                if devices.isEmpty { Text("None").foregroundColor(.secondary) }
                ForEach(devices.indices, id: \.self) { i in
                    HStack { Text(devices[i]["name"] as? String ?? ""); Spacer(); Text((devices[i]["adapter"] as? Bool ?? false) ? "Serial adapter" : "MIDI only").font(.caption).foregroundColor(.secondary) }
                }
            }
        }
        .navigationTitle("USB Serial")
        .onAppear { bridge.start(); refresh() }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("LRMIDISerialChanged"))) { _ in refresh() }
    }
    private func refresh() { links = bridge.links; devices = bridge.devices }
}
