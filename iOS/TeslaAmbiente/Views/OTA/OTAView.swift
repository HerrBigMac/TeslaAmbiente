// MARK: - OTAView.swift
import SwiftUI
import UniformTypeIdentifiers

struct OTAView: View {
    @EnvironmentObject var vm: MainViewModel
    @StateObject private var otaVM = OTAViewModel()
    @ObservedObject var ble = BLEManager.shared

    var body: some View {
        NavigationStack {
            ZStack {
                LinearGradient(colors:[Color(red:0.06,green:0.06,blue:0.1),Color(red:0.02,green:0.02,blue:0.06)],startPoint:.top,endPoint:.bottom).ignoresSafeArea()
                if !otaVM.isDeveloperUnlocked { lockScreen } else { developerContent }
            }
            .navigationTitle("Developer").navigationBarTitleDisplayMode(.large)
            .toolbarBackground(.ultraThinMaterial, for: .navigationBar)
            .toolbar { if otaVM.isDeveloperUnlocked { ToolbarItem(placement: .primaryAction) { Button { withAnimation { otaVM.isDeveloperUnlocked=false; otaVM.reset() } } label: { Image(systemName:"lock.fill").foregroundStyle(.secondary) } } } }
            .sheet(isPresented: $otaVM.isShowingFilePicker) { DocumentPicker { url in otaVM.loadFirmware(from: url) } }
        }
    }

    private var lockScreen: some View {
        VStack(spacing: 40) {
            Spacer()
            VStack(spacing: 16) {
                ZStack { Circle().fill(Color.accentColor.opacity(0.1)).frame(width:100,height:100); Image(systemName:"lock.shield.fill").font(.system(size:44)).symbolRenderingMode(.hierarchical).foregroundStyle(.accentColor) }
                VStack(spacing: 8) { Text("Developer-Bereich").font(.title2.weight(.bold)); Text("Dieser Bereich ist passwortgeschützt.\nFirmware-Updates sind irreversibel.").font(.subheadline).foregroundStyle(.secondary).multilineTextAlignment(.center) }
            }
            GlassCard(padding: 24) {
                VStack(spacing: 16) {
                    SecureField("Developer-Passwort",text:$otaVM.passwordInput).textFieldStyle(.roundedBorder).autocorrectionDisabled().onSubmit { otaVM.checkPassword() }
                    if otaVM.wrongPassword { Label("Falsches Passwort",systemImage:"xmark.circle.fill").font(.caption).foregroundStyle(.red).transition(.opacity.combined(with:.scale)) }
                    Button { otaVM.checkPassword() } label: { Text("Entsperren").font(.body.weight(.semibold)).frame(maxWidth:.infinity).padding(.vertical,12).background(Color.accentColor).foregroundStyle(.white).clipShape(RoundedRectangle(cornerRadius:12)) }
                }
            }.padding(.horizontal, 32).animation(.spring(response: 0.3), value: otaVM.wrongPassword)
            Spacer(); Spacer()
        }
    }

    private var developerContent: some View {
        ScrollView {
            VStack(spacing: 16) {
                deviceInfoCard; targetSelectionCard; firmwarePickerCard
                if otaVM.isUploading || ble.otaStatus.isActive { otaProgressCard } else if otaVM.firmwareData != nil { uploadStartCard }
                rollbackCard
            }.padding(.horizontal, 16).padding(.top, 8).padding(.bottom, 32)
        }
    }

    private var deviceInfoCard: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 12) {
                Label("Gerätinfo",systemImage:"cpu").font(.headline.weight(.semibold)); Divider()
                if ble.connectionState.isConnected {
                    HStack { infoItem(label:"Firmware",value:ble.deviceInfo.firmwareVersion); Spacer(); infoItem(label:"Freier RAM",value:"\(ble.deviceInfo.freeHeap/1024) KB"); Spacer(); infoItem(label:"Uptime",value:uptimeString(ble.deviceInfo.uptime)) }
                    Divider()
                    HStack { infoItem(label:"ESP-NOW OK",value:"\(ble.deviceInfo.espNowOK)"); Spacer(); infoItem(label:"ESP-NOW Fehler",value:"\(ble.deviceInfo.espNowFail)") }
                    Button { ble.requestDeviceInfo() } label: { Label("Aktualisieren",systemImage:"arrow.clockwise").font(.caption).foregroundStyle(.accentColor) }
                } else { Label("Nicht verbunden",systemImage:"bluetooth.slash").font(.subheadline).foregroundStyle(.secondary) }
            }
        }
    }

    private var targetSelectionCard: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 12) {
                Label("Zielgerät",systemImage:"cpu.fill").font(.headline.weight(.semibold)); Divider()
                LazyVGrid(columns:[GridItem(.flexible()),GridItem(.flexible())],spacing:10) {
                    ForEach(OTATarget.allCases) { target in
                        Button { otaVM.selectedTarget=target } label: {
                            HStack(spacing:8) { Image(systemName:target.systemIcon).font(.subheadline); Text(target.displayName).font(.caption.weight(.medium)).lineLimit(2).multilineTextAlignment(.leading) }
                            .frame(maxWidth:.infinity,alignment:.leading).padding(.horizontal,12).padding(.vertical,10)
                            .background(otaVM.selectedTarget==target ? Color.accentColor.opacity(0.15) : Color.secondary.opacity(0.08))
                            .foregroundStyle(otaVM.selectedTarget==target ? .accentColor : .secondary)
                            .clipShape(RoundedRectangle(cornerRadius:10))
                            .overlay(RoundedRectangle(cornerRadius:10).stroke(otaVM.selectedTarget==target ? Color.accentColor.opacity(0.5) : Color.clear,lineWidth:1.5))
                        }.buttonStyle(.plain)
                    }
                }
            }
        }
    }

    private var firmwarePickerCard: some View {
        GlassCard {
            VStack(alignment: .leading, spacing: 12) {
                Label("Firmware-Datei",systemImage:"doc.fill").font(.headline.weight(.semibold)); Divider()
                if let url=otaVM.firmwareURL {
                    HStack(spacing:12) { Image(systemName:"checkmark.circle.fill").foregroundStyle(.green).font(.title3); VStack(alignment:.leading,spacing:2) { Text(url.lastPathComponent).font(.subheadline.weight(.medium)).lineLimit(1); Text(otaVM.firmwareSize).font(.caption).foregroundStyle(.secondary) }; Spacer(); Button { otaVM.reset() } label: { Image(systemName:"xmark.circle.fill").foregroundStyle(.secondary) } }
                } else {
                    Button { otaVM.isShowingFilePicker=true } label: {
                        HStack { Image(systemName:"plus.circle.fill").font(.title3); VStack(alignment:.leading,spacing:2) { Text("Firmware auswählen").font(.body.weight(.medium)); Text(".bin oder .hex Datei").font(.caption).foregroundStyle(.secondary) }; Spacer(); Image(systemName:"chevron.right").font(.caption).foregroundStyle(.secondary) }.padding(.vertical,4)
                    }.foregroundStyle(.accentColor)
                }
            }
        }
    }

    private var uploadStartCard: some View {
        GlassCard {
            VStack(spacing: 16) {
                HStack { VStack(alignment:.leading,spacing:4) { Text("Bereit zum Flashen").font(.headline.weight(.semibold)); Text("Ziel: \(otaVM.selectedTarget.displayName)").font(.caption).foregroundStyle(.secondary) }; Spacer(); Image(systemName:"checkmark.circle.fill").foregroundStyle(.green).font(.title2) }
                Label { Text("Stelle sicher, dass das Gerät eingeschaltet und die Verbindung stabil ist. Den Prozess nicht unterbrechen.").font(.caption) } icon: { Image(systemName:"exclamationmark.triangle.fill").foregroundStyle(.orange) }.padding(10).background(Color.orange.opacity(0.1)).clipShape(RoundedRectangle(cornerRadius:10))
                Button { otaVM.startUpload() } label: { HStack(spacing:8) { Image(systemName:"bolt.fill"); Text("Firmware übertragen").font(.body.weight(.semibold)) }.frame(maxWidth:.infinity).padding(.vertical,14).background(LinearGradient(colors:[.orange,.red],startPoint:.leading,endPoint:.trailing)).foregroundStyle(.white).clipShape(RoundedRectangle(cornerRadius:14)) }.disabled(!otaVM.canStartUpload)
            }
        }
    }

    private var otaProgressCard: some View {
        GlassCard {
            VStack(spacing: 16) {
                HStack { Text(phaseTitle).font(.headline.weight(.semibold)); Spacer(); if case .uploading = ble.otaStatus.phase { Button { otaVM.abortUpload() } label: { Label("Abbrechen",systemImage:"xmark.circle.fill").font(.caption).foregroundStyle(.red) } } }
                VStack(spacing: 8) { ProgressView(value:progressValue).tint(progressColor).scaleEffect(y:2); HStack { Text(phaseSubtitle).font(.caption).foregroundStyle(.secondary); Spacer(); if case .uploading(let p)=ble.otaStatus.phase { Text("\(Int(p*100))%").font(.caption.monospacedDigit().weight(.semibold)) } } }
                if case .success = ble.otaStatus.phase { successBanner } else if case .failed(let msg) = ble.otaStatus.phase { errorBanner(message: msg) }
            }
        }
    }

    private var rollbackCard: some View {
        GlassCard(padding: 14) {
            HStack(spacing:12) { Image(systemName:"arrow.uturn.backward.circle.fill").font(.title3).symbolRenderingMode(.hierarchical).foregroundStyle(.orange); VStack(alignment:.leading,spacing:3) { Text("Rollback").font(.subheadline.weight(.semibold)); Text("Bei Fehlern: Altes Firmware-Binary erneut flashen. ESP32 OTA unterstützt automatisch Boot-Fallback bei Absturz in der ersten Startphase.").font(.caption).foregroundStyle(.secondary) } }
        }
    }

    private var phaseTitle: String { switch ble.otaStatus.phase { case .preparing: return "Vorbereitung..."; case .uploading: return "Übertragung läuft"; case .verifying: return "Verifizierung..."; case .success: return "Erfolgreich!"; case .failed: return "Fehler beim Update"; case .rollingBack: return "Rollback..."; default: return "Bereit" } }
    private var phaseSubtitle: String { switch ble.otaStatus.phase { case .uploading(let p): return "\(Int(p*Double(ble.otaStatus.firmwareSize)/1024)) / \(ble.otaStatus.firmwareSize/1024) KB"; case .verifying: return "CRC-Prüfung läuft..."; case .success: return "Gerät startet neu"; case .failed(let msg): return msg; default: return "" } }
    private var progressValue: Double { switch ble.otaStatus.phase { case .uploading(let p): return p; case .verifying,.success: return 1.0; default: return 0 } }
    private var progressColor: Color { switch ble.otaStatus.phase { case .success: return .green; case .failed: return .red; default: return .orange } }
    private var successBanner: some View { HStack(spacing:10) { Image(systemName:"checkmark.circle.fill").foregroundStyle(.green).font(.title3); VStack(alignment:.leading,spacing:2) { Text("Update erfolgreich!").font(.subheadline.weight(.semibold)).foregroundStyle(.green); Text("Das Gerät startet mit der neuen Firmware neu.").font(.caption).foregroundStyle(.secondary) } }.padding(12).background(Color.green.opacity(0.1)).clipShape(RoundedRectangle(cornerRadius:10)) }
    private func errorBanner(message: String) -> some View { HStack(spacing:10) { Image(systemName:"xmark.circle.fill").foregroundStyle(.red).font(.title3); VStack(alignment:.leading,spacing:2) { Text("Update fehlgeschlagen").font(.subheadline.weight(.semibold)).foregroundStyle(.red); Text(message).font(.caption).foregroundStyle(.secondary) } }.padding(12).background(Color.red.opacity(0.1)).clipShape(RoundedRectangle(cornerRadius:10)) }
    private func infoItem(label: String, value: String) -> some View { VStack(alignment:.leading,spacing:3) { Text(label).font(.caption2).foregroundStyle(.secondary); Text(value).font(.caption.weight(.semibold).monospacedDigit()) } }
    private func uptimeString(_ seconds: UInt32) -> String { let h=seconds/3600; let m=(seconds%3600)/60; return "\(h)h \(m)m" }
}

struct DocumentPicker: UIViewControllerRepresentable {
    let completion: (URL) -> Void
    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let types: [UTType] = [UTType(filenameExtension:"bin") ?? .data, UTType(filenameExtension:"hex") ?? .data, .data]
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: types)
        picker.delegate = context.coordinator; picker.allowsMultipleSelection = false; return picker
    }
    func updateUIViewController(_ uiViewController: UIDocumentPickerViewController, context: Context) {}
    func makeCoordinator() -> Coordinator { Coordinator(completion: completion) }
    class Coordinator: NSObject, UIDocumentPickerDelegate {
        let completion: (URL) -> Void
        init(completion: @escaping (URL) -> Void) { self.completion = completion }
        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) { guard let url=urls.first else { return }; completion(url) }
    }
}