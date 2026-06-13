// MARK: - LEDControlView.swift
import SwiftUI

extension LEDEffect {
    var supportsMultiColor: Bool { self == .police || self == .softFade || self == .twoColorFade || self == .threeColorFade || self == .blindSpot }
}

struct LEDControlView: View {
    @EnvironmentObject var vm: MainViewModel
    @ObservedObject var ble = BLEManager.shared

    var body: some View {
        NavigationStack {
            ZStack {
                LinearGradient(colors: [Color(red:0.06,green:0.06,blue:0.1),Color(red:0.02,green:0.02,blue:0.06)], startPoint:.top, endPoint:.bottom).ignoresSafeArea()
                ScrollView {
                    VStack(spacing: 16) {
                        zoneSelector; powerToggle; colorSection; effectSection
                        if vm.ledSettings.powerOn { parameterSection }
                        ledRangeSection; sendButton
                    }
                    .padding(.horizontal, 16).padding(.top, 8).padding(.bottom, 32)
                }
            }
            .navigationTitle("LED Steuerung").navigationBarTitleDisplayMode(.large)
            .toolbarBackground(.ultraThinMaterial, for: .navigationBar)
            .overlay(alignment: .bottom) {
                if let f = vm.showFeedback { FeedbackBanner(message: f.message, isError: f.isError).transition(.move(edge:.bottom).combined(with:.opacity)).padding(.bottom, 8) }
            }
            .animation(.spring(response: 0.3), value: vm.showFeedback != nil)
        }
    }

    private var zoneSelector: some View {
        GlassCard(padding: 14) {
            VStack(alignment: .leading, spacing: 10) {
                Text("Zone auswählen").font(.subheadline.weight(.semibold)).foregroundStyle(.secondary)
                ScrollView(.horizontal, showsIndicators: false) { HStack(spacing: 10) { ForEach(LEDZone.allCases) { zone in ZoneButton(zone:zone,isSelected:vm.selectedZone==zone) { vm.selectedZone=zone } } } }
            }
        }
    }

    private var powerToggle: some View {
        GlassCard(padding: 14) {
            HStack {
                Label { VStack(alignment:.leading,spacing:2) { Text("LEDs").font(.body.weight(.semibold)); Text(vm.ledSettings.powerOn ? "Eingeschaltet":"Ausgeschaltet").font(.caption).foregroundStyle(.secondary) } } icon: { Image(systemName:vm.ledSettings.powerOn ? "lightbulb.fill":"lightbulb.slash.fill").symbolRenderingMode(.hierarchical).foregroundStyle(vm.ledSettings.powerOn ? .yellow:.secondary).font(.title3).frame(width:28) }
                Spacer()
                Toggle("", isOn: $vm.ledSettings.powerOn).labelsHidden().onChange(of: vm.ledSettings.powerOn) { _,on in if on { vm.turnOn() } else { vm.turnOff() } }
            }
        }
    }

    private let quickColors: [(String, LEDColor)] = [("Rot",LEDColor(r:220,g:20,b:20)),("Grün",LEDColor(r:0,g:200,b:30)),("Blau",LEDColor(r:0,g:60,b:255)),("Weiß",LEDColor(r:255,g:230,b:200)),("Orange",LEDColor(r:255,g:100,b:0)),("Lila",LEDColor(r:150,g:0,b:220)),("Cyan",LEDColor(r:0,g:220,b:220)),("Pink",LEDColor(r:255,g:20,b:100))]

    private var colorSection: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Text("Farben").font(.headline.weight(.semibold))
                    Spacer()
                    HStack(spacing: 6) { ColorCircle(color:vm.ledSettings.color1,size:28); if vm.ledSettings.effect.supportsMultiColor { ColorCircle(color:vm.ledSettings.color2,size:28); if vm.ledSettings.effect == .threeColorFade { ColorCircle(color:vm.ledSettings.color3,size:28) } } }
                }
                Divider()
                LEDColorPicker(label: "Farbe 1 (Primär)", color: $vm.ledSettings.color1)
                if vm.ledSettings.effect.supportsMultiColor {
                    LEDColorPicker(label: "Farbe 2", color: $vm.ledSettings.color2)
                    if vm.ledSettings.effect == .threeColorFade { LEDColorPicker(label: "Farbe 3", color: $vm.ledSettings.color3) }
                }
                HStack(spacing: 10) {
                    Text("Schnellfarben:").font(.caption).foregroundStyle(.secondary)
                    ScrollView(.horizontal, showsIndicators: false) { HStack(spacing: 8) { ForEach(quickColors, id: \.0) { (name, color) in Button { vm.ledSettings.color1 = color } label: { VStack(spacing:3) { ColorCircle(color:color,size:28); Text(name).font(.system(size:8)).foregroundStyle(.secondary) } }.buttonStyle(.plain) } } }
                }
            }
        }
    }

    private var effectSection: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 12) {
                HStack { Text("Effekt").font(.headline.weight(.semibold)); Spacer(); Text(vm.ledSettings.effect.displayName).font(.subheadline).foregroundStyle(.accentColor) }
                Divider()
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 75), spacing: 10)], spacing: 10) {
                    ForEach(LEDEffect.allCases) { effect in
                        EffectButton(effect:effect, isSelected:vm.ledSettings.effect==effect) { vm.ledSettings.effect=effect; if effect == .off { vm.ledSettings.powerOn=false } else { vm.ledSettings.powerOn=true } }
                    }
                }
            }
        }
    }

    private var parameterSection: some View {
        GlassCard {
            VStack(spacing: 16) {
                Text("Parameter").font(.headline.weight(.semibold)).frame(maxWidth: .infinity, alignment: .leading)
                Divider()
                LabeledSlider(label:"Helligkeit",icon:"sun.max.fill",value:$vm.ledSettings.brightness,color:.yellow)
                if vm.ledSettings.effect.supportsSpeed { LabeledSlider(label:"Geschwindigkeit",icon:"speedometer",value:$vm.ledSettings.speed,color:.orange) }
                if vm.ledSettings.effect.supportsIntensity { LabeledSlider(label:"Intensität",icon:"dial.high.fill",value:$vm.ledSettings.intensity,color:.purple) }
            }
        }
    }

    private var ledRangeSection: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 12) {
                HStack { Label("LED-Bereich",systemImage:"ruler.fill").font(.headline.weight(.semibold)); Spacer(); Text("\(vm.ledSettings.ledStart)–\(vm.ledSettings.ledEnd)").font(.subheadline.monospacedDigit()).foregroundStyle(.secondary) }
                Divider()
                VStack(spacing: 12) {
                    HStack { Text("Start").font(.subheadline).foregroundStyle(.secondary).frame(width:45,alignment:.leading); Slider(value:Binding(get:{Double(vm.ledSettings.ledStart)},set:{vm.ledSettings.ledStart=UInt16($0)}),in:0...Double(vm.ledSettings.ledEnd)).tint(.accentColor); Text("\(vm.ledSettings.ledStart)").font(.caption.monospacedDigit()).foregroundStyle(.secondary).frame(width:30) }
                    HStack { Text("Ende").font(.subheadline).foregroundStyle(.secondary).frame(width:45,alignment:.leading); Slider(value:Binding(get:{Double(vm.ledSettings.ledEnd)},set:{vm.ledSettings.ledEnd=UInt16($0)}),in:Double(vm.ledSettings.ledStart)...129).tint(.accentColor); Text("\(vm.ledSettings.ledEnd)").font(.caption.monospacedDigit()).foregroundStyle(.secondary).frame(width:30) }
                }
                Button { vm.ledSettings.ledStart=0; vm.ledSettings.ledEnd=129 } label: { Label("Gesamten Streifen",systemImage:"arrow.left.and.right").font(.caption).foregroundStyle(.secondary) }
            }
        }
    }

    private var sendButton: some View {
        Button { vm.sendCurrentSettings() } label: {
            HStack(spacing: 10) { Image(systemName: "paperplane.fill"); Text("An \(vm.selectedZone.displayName) senden").font(.body.weight(.semibold)) }
            .frame(maxWidth: .infinity).padding(.vertical, 16)
            .background(ble.connectionState.isConnected ? LinearGradient(colors:[.accentColor,.accentColor.opacity(0.7)],startPoint:.leading,endPoint:.trailing) : LinearGradient(colors:[.secondary.opacity(0.3),.secondary.opacity(0.2)],startPoint:.leading,endPoint:.trailing))
            .foregroundStyle(.white).clipShape(RoundedRectangle(cornerRadius: 16))
            .shadow(color: ble.connectionState.isConnected ? .accentColor.opacity(0.4) : .clear, radius: 8, y: 4)
        }.disabled(!ble.connectionState.isConnected).animation(.easeInOut(duration: 0.2), value: ble.connectionState.isConnected)
    }
}

struct LEDColorPicker: View {
    let label: String; @Binding var color: LEDColor
    private var swiftColor: Binding<Color> {
        Binding(get:{Color(red:Double(color.r)/255,green:Double(color.g)/255,blue:Double(color.b)/255)},
                set:{ let r=$0.resolve(in:.init()); color=LEDColor(r:UInt8(clamping:Int(r.red*255)),g:UInt8(clamping:Int(r.green*255)),b:UInt8(clamping:Int(r.blue*255))) })
    }
    var body: some View {
        HStack(spacing: 12) {
            ColorCircle(color: color, size: 32)
            VStack(alignment: .leading, spacing: 2) { Text(label).font(.subheadline.weight(.medium)); Text("R:\(color.r) G:\(color.g) B:\(color.b)").font(.caption.monospacedDigit()).foregroundStyle(.secondary) }
            Spacer()
            ColorPicker("", selection: swiftColor, supportsOpacity: false).labelsHidden().frame(width: 44, height: 30)
        }
    }
}