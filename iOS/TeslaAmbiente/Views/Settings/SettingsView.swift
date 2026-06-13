// MARK: - SettingsView.swift
import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var vm: MainViewModel
    @ObservedObject var ble = BLEManager.shared

    var body: some View {
        NavigationStack {
            ZStack {
                Color(UIColor.systemGroupedBackground).ignoresSafeArea()
                List {
                    dashBasicSection; brightnessSection; autopilotSection
                    blindSpotSection; animationSection; blinkerSection; sendSection
                }
                .listStyle(.insetGrouped)
                .navigationTitle("Einstellungen").navigationBarTitleDisplayMode(.large)
                .toolbar { ToolbarItem(placement: .primaryAction) { Button { vm.sendFeatureSettings() } label: { Label("Speichern",systemImage:"checkmark.circle.fill").symbolRenderingMode(.hierarchical) }.disabled(!ble.connectionState.isConnected) } }
            }
            .overlay(alignment: .bottom) { if let f=vm.showFeedback { FeedbackBanner(message:f.message,isError:f.isError).transition(.move(edge:.bottom).combined(with:.opacity)).padding(.bottom, 8) } }
            .animation(.spring(response: 0.3), value: vm.showFeedback != nil)
        }
    }

    private var dashBasicSection: some View {
        Section {
            NavigationLink { ColorDetailView(title:"Dashboard Basisfarbe",color:$vm.featureSettings.baseColor) } label: {
                HStack { Label { Text("Basisfarbe") } icon: { Image(systemName:"paintpalette.fill").foregroundStyle(.accentColor) }; Spacer(); ColorCircle(color:vm.featureSettings.baseColor,size:26) }
            }
            HStack {
                Label { Text("Anzahl LEDs") } icon: { Image(systemName:"number.circle.fill").foregroundStyle(.accentColor) }
                Spacer()
                Stepper("\(vm.featureSettings.dashLedCount)",value:Binding(get:{Int(vm.featureSettings.dashLedCount)},set:{vm.featureSettings.dashLedCount=UInt16($0)}),in:1...144,step:1).fixedSize()
            }
            FeatureToggleRow(title:"Dashboard AUS",subtitle:"Dashboard-LEDs komplett deaktivieren",icon:"moon.fill",isOn:$vm.featureSettings.dashPowerOff)
        } header: { Text("Dashboard") } footer: { Text("Die Basisfarbe der Dashboard-LEDs im Normalbetrieb.") }
    }

    private var brightnessSection: some View {
        Section {
            FeatureToggleRow(title:"Auto-Helligkeit",subtitle:"Helligkeit folgt der Fahrzeugdisplay-Helligkeit",icon:"sun.and.horizon.fill",isOn:$vm.featureSettings.autoBrightness)
            if !vm.featureSettings.autoBrightness {
                VStack(alignment: .leading, spacing: 8) {
                    HStack { Label { Text("Manuelle Helligkeit") } icon: { Image(systemName:"sun.max.fill").foregroundStyle(.yellow) }; Spacer(); Text("\(vm.featureSettings.manualBrightness)").font(.subheadline.monospacedDigit()).foregroundStyle(.secondary) }
                    Slider(value:Binding(get:{Double(vm.featureSettings.manualBrightness)},set:{vm.featureSettings.manualBrightness=UInt8($0)}),in:0...255).tint(.yellow)
                }
            }
        } header: { Text("Helligkeit") }
    }

    private var autopilotSection: some View {
        Section {
            FeatureToggleRow(title:"Autopilot-Anzeige",subtitle:"Dashboard wechselt zur Autopilot-Farbe",icon:"brain.head.profile",isOn:$vm.featureSettings.autopilotDashEnabled)
            if vm.featureSettings.autopilotDashEnabled {
                NavigationLink { ColorDetailView(title:"Autopilot-Farbe",color:$vm.featureSettings.autopilotColor) } label: {
                    HStack { Label { Text("Autopilot-Farbe") } icon: { Image(systemName:"circle.fill").foregroundStyle(.accentColor) }; Spacer(); ColorCircle(color:vm.featureSettings.autopilotColor,size:26) }
                }
            }
        } header: { Text("Autopilot") } footer: { Text("Wenn Autopilot aktiv ist, leuchtet das Dashboard in der konfigurierten Farbe.") }
    }

    private var blindSpotSection: some View {
        Section {
            FeatureToggleRow(title:"Totwinkel-Warnung",subtitle:"LEDs zeigen Totwinkel-Objekte",icon:"eye.trianglebadge.exclamationmark",isOn:$vm.featureSettings.blindSpotDashEnabled)
            if vm.featureSettings.blindSpotDashEnabled {
                FeatureToggleRow(title:"Nur mit Blinker",subtitle:"Totwinkel nur bei aktivem Blinker anzeigen",icon:"arrow.triangle.turn.up.right.diamond.fill",isOn:$vm.featureSettings.blindSpotOnlyWithBlinker)
                HStack { Label { Text("Segmentgröße") } icon: { Image(systemName:"slider.horizontal.3").foregroundStyle(.orange) }; Spacer(); Stepper("\(vm.featureSettings.blindSpotDashPercent)%",value:Binding(get:{Int(vm.featureSettings.blindSpotDashPercent)},set:{vm.featureSettings.blindSpotDashPercent=UInt8($0)}),in:5...50,step:5).fixedSize() }
            }
        } header: { Text("Totwinkel") } footer: { Text("Prozentsatz der LEDs, die für die Totwinkel-Warnung genutzt werden (pro Seite).") }
    }

    private var animationSection: some View {
        Section {
            FeatureToggleRow(title:"Willkommens-Animation",subtitle:"LEDs öffnen sich beim Aufwachen",icon:"sun.horizon.fill",isOn:$vm.featureSettings.welcomeAnimationEnabled)
            FeatureToggleRow(title:"Abschied-Animation",subtitle:"LEDs schließen sich beim Schlafen",icon:"moon.stars.fill",isOn:$vm.featureSettings.goodbyeAnimationEnabled)
            FeatureToggleRow(title:"Tür-Highlight",subtitle:"Offene Türen orange hervorheben",icon:"door.left.hand.open",isOn:$vm.featureSettings.doorOpenHighlightEnabled)
            FeatureToggleRow(title:"Lade-Animation",subtitle:"Ladebalken beim Laden anzeigen",icon:"battery.100.bolt",isOn:$vm.featureSettings.chargeDashEnabled)
        } header: { Text("Animationen & Features") }
    }

    private var blinkerSection: some View {
        Section {
            FeatureToggleRow(title:"Blinker-Anzeige",subtitle:"Dashboard zeigt aktiven Blinker",icon:"arrow.turn.up.right",isOn:$vm.featureSettings.blinkerDashEnabled)
            if vm.featureSettings.blinkerDashEnabled {
                HStack { Label { Text("Blinker-Segmentgröße") } icon: { Image(systemName:"slider.horizontal.3").foregroundStyle(.orange) }; Spacer(); Stepper("\(vm.featureSettings.blinkerDashPercent)%",value:Binding(get:{Int(vm.featureSettings.blinkerDashPercent)},set:{vm.featureSettings.blinkerDashPercent=UInt8($0)}),in:5...50,step:5).fixedSize() }
            }
        } header: { Text("Blinker") }
    }

    private var sendSection: some View {
        Section {
            Button { vm.sendFeatureSettings() } label: {
                HStack { Spacer(); Label("Alle Einstellungen übertragen",systemImage:"paperplane.fill").font(.body.weight(.semibold)).foregroundStyle(ble.connectionState.isConnected ? .white:.secondary); Spacer() }.padding(.vertical, 4)
            }
            .listRowBackground(ble.connectionState.isConnected ? Color.accentColor : Color.secondary.opacity(0.2))
            .disabled(!ble.connectionState.isConnected)
        } footer: { if !ble.connectionState.isConnected { Label("Nicht verbunden — Einstellungen werden erst beim Verbinden übertragen.",systemImage:"exclamationmark.triangle").font(.caption).foregroundStyle(.orange) } }
    }
}

struct ColorDetailView: View {
    let title: String; @Binding var color: LEDColor
    @Environment(\.dismiss) var dismiss
    private var swiftColor: Binding<Color> {
        Binding(get:{Color(red:Double(color.r)/255,green:Double(color.g)/255,blue:Double(color.b)/255)},
                set:{ let r=$0.resolve(in:.init()); color=LEDColor(r:UInt8(clamping:Int(r.red*255)),g:UInt8(clamping:Int(r.green*255)),b:UInt8(clamping:Int(r.blue*255))) })
    }
    var body: some View {
        List {
            Section { HStack { Spacer(); ColorCircle(color:color,size:80); Spacer() }.listRowBackground(Color.clear).padding(.vertical, 12); ColorPicker("Farbe wählen",selection:swiftColor,supportsOpacity:false) }
            Section("RGB-Werte") { LabeledContent("Rot",value:"\(color.r)"); LabeledContent("Grün",value:"\(color.g)"); LabeledContent("Blau",value:"\(color.b)"); LabeledContent("Hex",value:String(format:"#%02X%02X%02X",color.r,color.g,color.b)) }
        }
        .navigationTitle(title).navigationBarTitleDisplayMode(.inline)
    }
}