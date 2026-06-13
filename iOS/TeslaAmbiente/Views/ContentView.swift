// MARK: - ContentView.swift
import SwiftUI

struct ContentView: View {
    @StateObject var vm = MainViewModel()
    @ObservedObject var ble = BLEManager.shared
    @State private var selectedTab: Int = 0
    var body: some View {
        TabView(selection: $selectedTab) {
            DashboardView()
                .tabItem { Label("Dashboard", systemImage: "gauge.with.needle.fill") }
                .tag(0)
            LEDControlView()
                .tabItem { Label("LEDs", systemImage: "lightbulb.fill") }
                .tag(1)
                .badge(ble.connectionState.isConnected ? nil : "!")
            SettingsView()
                .tabItem { Label("Features", systemImage: "slider.vertical.3") }
                .tag(2)
            OTAView()
                .tabItem { Label("Developer", systemImage: "terminal.fill") }
                .tag(3)
        }
        .environmentObject(vm)
        .tint(Color.accentColor)
        .preferredColorScheme(.dark)
    }
}

#Preview { ContentView() }