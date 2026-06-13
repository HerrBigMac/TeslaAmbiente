// MARK: - TeslaAmbienteApp.swift
import SwiftUI

@main
struct TeslaAmbienteApp: App {
    @Environment(\.scenePhase) var scenePhase
    var body: some Scene {
        WindowGroup {
            ContentView()
                .preferredColorScheme(.dark)
        }
        .onChange(of: scenePhase) { _, newPhase in
            switch newPhase {
            case .active:
                if case .disconnected = BLEManager.shared.connectionState { }
            default: break
            }
        }
    }
}