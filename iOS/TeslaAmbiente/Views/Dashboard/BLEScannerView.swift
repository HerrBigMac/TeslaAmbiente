// MARK: - BLEScannerView.swift
import SwiftUI

struct BLEScannerView: View {
    @ObservedObject var ble = BLEManager.shared
    @Environment(\.dismiss) var dismiss
    @State private var animateScanning = false

    var body: some View {
        NavigationStack {
            ZStack {
                Color(UIColor.systemGroupedBackground).ignoresSafeArea()
                VStack(spacing: 0) {
                    scanHeader
                    if ble.discoveredPeripherals.isEmpty { emptyStateView } else { deviceList }
                }
            }
            .navigationTitle("Gerät verbinden")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Schließen") { ble.stopScanning(); dismiss() } }
                ToolbarItem(placement: .primaryAction) { Button { ble.startScanning() } label: { Image(systemName: "arrow.clockwise") }.disabled(ble.connectionState == .scanning) }
            }
            .onAppear { ble.startScanning() }
            .onDisappear { ble.stopScanning() }
            .onChange(of: ble.connectionState.isConnected) { _, connected in if connected { dismiss() } }
        }
    }

    private var scanHeader: some View {
        VStack(spacing: 20) {
            ZStack {
                ForEach(0..<3, id: \.self) { i in
                    Circle().stroke(Color.accentColor.opacity(0.3 - Double(i)*0.08), lineWidth: 1.5)
                    .frame(width: CGFloat(80+i*40), height: CGFloat(80+i*40))
                    .scaleEffect(animateScanning ? 1.2 : 0.9)
                    .opacity(animateScanning ? 0 : 1)
                    .animation(.easeOut(duration: 1.5).repeatForever(autoreverses: false).delay(Double(i)*0.4), value: animateScanning)
                }
                Circle().fill(Color.accentColor.opacity(0.15)).frame(width: 70, height: 70)
                Image(systemName: "bluetooth").font(.system(size: 32, weight: .medium)).foregroundStyle(.accentColor)
            }
            .frame(height: 140).onAppear { animateScanning = true }
            VStack(spacing: 6) {
                Text(scanStatusText).font(.headline.weight(.semibold))
                if case .scanning = ble.connectionState {
                    HStack(spacing: 8) { ProgressView().scaleEffect(0.8); Text("Suche nach Tesla Ambiente...").font(.subheadline).foregroundStyle(.secondary) }
                }
            }
        }
        .padding(.vertical, 24).frame(maxWidth: .infinity).background(.ultraThinMaterial)
    }

    private var scanStatusText: String {
        switch ble.connectionState {
        case .scanning: return "Suche läuft..."
        case .connecting: return "Verbinde..."
        case .connected: return "Verbunden!"
        case .failed(let e): return "Fehler: \(e)"
        default: return "Bereit"
        }
    }

    private var emptyStateView: some View {
        VStack(spacing: 16) {
            Spacer()
            Image(systemName: "antenna.radiowaves.left.and.right.slash").font(.system(size: 50)).symbolRenderingMode(.hierarchical).foregroundStyle(.secondary)
            Text("Keine Geräte gefunden").font(.title3.weight(.semibold))
            Text("Stelle sicher, dass der Master-ESP32 eingeschaltet ist und BLE aktiviert hat.").font(.subheadline).foregroundStyle(.secondary).multilineTextAlignment(.center).padding(.horizontal, 40)
            Button { ble.startScanning() } label: { Label("Erneut suchen", systemImage: "arrow.clockwise").font(.body.weight(.semibold)).frame(maxWidth: 200).padding(.vertical, 12).background(Color.accentColor).foregroundStyle(.white).clipShape(RoundedRectangle(cornerRadius: 12)) }.padding(.top, 8)
            Spacer()
        }
    }

    private var deviceList: some View {
        List {
            Section { ForEach(ble.discoveredPeripherals.sorted(by: { $0.rssi > $1.rssi })) { device in DeviceRow(device: device) { ble.connect(to: device) } } } header: { Text("\(ble.discoveredPeripherals.count) Gerät(e) gefunden") }
        }.listStyle(.insetGrouped)
    }
}

struct DeviceRow: View {
    let device: BLEManager.DiscoveredDevice; let onConnect: () -> Void
    var signalColor: Color { switch device.rssi { case -50...: return .green; case -70..<(-50): return .orange; default: return .red } }
    var body: some View {
        HStack(spacing: 14) {
            ZStack { Circle().fill(Color.accentColor.opacity(0.15)).frame(width: 44, height: 44); Image(systemName: "cpu").font(.system(size: 20)).foregroundStyle(.accentColor) }
            VStack(alignment: .leading, spacing: 3) { Text(device.name).font(.body.weight(.semibold)); Text(device.peripheral.identifier.uuidString.prefix(8) + "...").font(.caption.monospaced()).foregroundStyle(.secondary) }
            Spacer()
            VStack(spacing: 2) { Image(systemName: "wifi").font(.system(size: 14)).foregroundStyle(signalColor); Text("\(device.rssi) dB").font(.system(size: 9)).foregroundStyle(.secondary) }
            Button(action: onConnect) { Text("Verbinden").font(.subheadline.weight(.semibold)).padding(.horizontal, 14).padding(.vertical, 7).background(Color.accentColor).foregroundStyle(.white).clipShape(Capsule()) }
        }.padding(.vertical, 4)
    }
}