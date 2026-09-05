import CoreBluetooth
import CoreFoundation
import Foundation

private let serviceUUID = CBUUID(string: "B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A001")
private let controlUUID = CBUUID(string: "B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A002")
private let statusUUID = CBUUID(string: "B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A003")
private let microphoneUUID = CBUUID(string: "B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A004")
private let speakerUUID = CBUUID(string: "B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A005")
private let deviceNamePrefix = "JUFF-"
private let maximumMessageBytes = 512

private let allowedControlTypes: Set<String> = [
    "status",
    "command",
    "provision",
    "juff.provision.v1",
    "device.ready",
    "voice.ready",
    "voice.deactivated",
    "audio.begin",
    "audio.done",
    "playback.clear",
    "input.suspend",
    "input.resume",
    "voice.state",
    "error",
]

private func emit(_ line: String) {
    FileHandle.standardOutput.write(Data((line + "\n").utf8))
}

private func oneLine(_ value: String) -> String {
    value.replacingOccurrences(of: "\r", with: " ")
        .replacingOccurrences(of: "\n", with: " ")
}

private func protocolUInt32(_ value: Any?) -> UInt32? {
    guard let number = value as? NSNumber,
          CFGetTypeID(number) != CFBooleanGetTypeID() else { return nil }
    let value = number.doubleValue
    guard value.isFinite, value >= 0, value <= Double(UInt32.max),
          value.rounded(.towardZero) == value else { return nil }
    return UInt32(value)
}

private func normalizedMessage(_ line: String) -> Data? {
    let source = line.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let input = source.data(using: .utf8),
          let object = try? JSONSerialization.jsonObject(with: input),
          let dictionary = object as? [String: Any],
          let type = dictionary["type"] as? String,
          allowedControlTypes.contains(type),
          let compact = try? JSONSerialization.data(
              withJSONObject: dictionary,
              options: [.sortedKeys]
          ) else {
        return nil
    }
    var framed = compact
    guard framed.count <= maximumMessageBytes else {
        return nil
    }
    framed.append(0x0A)
    return framed
}

private func chunks(of data: Data, maximumLength: Int) -> [Data] {
    guard !data.isEmpty, maximumLength > 0 else {
        return []
    }
    var result: [Data] = []
    var offset = 0
    while offset < data.count {
        let end = min(offset + maximumLength, data.count)
        result.append(data.subdata(in: offset..<end))
        offset = end
    }
    return result
}

private func linearPCMToMuLaw(_ pcm: Data) -> Data {
    var output = [UInt8]()
    output.reserveCapacity(pcm.count / 2)
    pcm.withUnsafeBytes { rawBuffer in
        let bytes = rawBuffer.bindMemory(to: UInt8.self)
        var offset = 0
        while offset + 1 < bytes.count {
            let bits = UInt16(bytes[offset]) | (UInt16(bytes[offset + 1]) << 8)
            var value = Int(Int16(bitPattern: bits))
            let sign = value < 0 ? 0x80 : 0
            if value < 0 {
                value = -value
            }
            value = min(value, 32635) + 0x84
            var exponent = 7
            var mask = 0x4000
            while exponent > 0, value & mask == 0 {
                exponent -= 1
                mask >>= 1
            }
            let mantissa = (value >> (exponent + 3)) & 0x0F
            output.append(UInt8(~(sign | (exponent << 4) | mantissa) & 0xFF))
            offset += 2
        }
    }
    return Data(output)
}

private func muLawToLinearPCM(_ encoded: Data) -> Data {
    var output = [UInt8](repeating: 0, count: encoded.count * 2)
    encoded.withUnsafeBytes { rawBuffer in
        let bytes = rawBuffer.bindMemory(to: UInt8.self)
        for index in bytes.indices {
            let value = Int(~bytes[index]) & 0xFF
            let sign = value & 0x80
            let exponent = (value >> 4) & 0x07
            let mantissa = value & 0x0F
            var sample = (((mantissa << 3) + 0x84) << exponent) - 0x84
            if sign != 0 {
                sample = -sample
            }
            let bits = UInt16(bitPattern: Int16(clamping: sample))
            output[index * 2] = UInt8(bits & 0xFF)
            output[index * 2 + 1] = UInt8(bits >> 8)
        }
    }
    return Data(output)
}

private func runSelfTest() -> Bool {
    guard protocolUInt32(NSNumber(value: UInt32.max)) == UInt32.max,
          protocolUInt32(NSNumber(value: true)) == nil,
          protocolUInt32(NSNumber(value: -1)) == nil,
          protocolUInt32(NSNumber(value: 1.5)) == nil,
          protocolUInt32("1") == nil else {
        emit("SELFTEST failed:flow-number")
        return false
    }
    guard serviceUUID.uuidString == "B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A001",
          controlUUID.uuidString == "B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A002",
          statusUUID.uuidString == "B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A003",
          microphoneUUID.uuidString == "B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A004",
          speakerUUID.uuidString == "B8A5FCA1-8A0F-4DB1-9C6C-7B5B6C49A005" else {
        emit("SELFTEST failed:uuid")
        return false
    }
    guard normalizedMessage("not-json") == nil,
          normalizedMessage("{\"type\":\"unknown\"}") == nil,
          let message = normalizedMessage("{\"type\":\"command\",\"name\":\"interrupt\"}"),
          message.last == 0x0A,
          normalizedMessage("{\"type\":\"audio.begin\",\"responseId\":\"test\"}") != nil else {
        emit("SELFTEST failed:message")
        return false
    }
    let payload = Data(repeating: 0x41, count: 500)
    let split = chunks(of: payload, maximumLength: 182)
    guard split.map(\.count) == [182, 182, 136],
          split.reduce(Data(), +) == payload else {
        emit("SELFTEST failed:chunks")
        return false
    }
    let pcm = Data([0x00, 0x00, 0xE8, 0x03, 0x18, 0xFC, 0x30, 0x75, 0xD0, 0x8A])
    let muLaw = linearPCMToMuLaw(pcm)
    let decoded = muLawToLinearPCM(muLaw)
    guard muLaw.count == pcm.count / 2,
          decoded.count == pcm.count,
          muLaw.first == 0xFF,
          decoded.prefix(2) == Data([0x00, 0x00]) else {
        emit("SELFTEST failed:mulaw")
        return false
    }
    emit("SELFTEST OK")
    return true
}

private final class JuffCentral: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private struct ControlWrite {
        let data: Data
        let type: String
        var token: SpeakerTransport.Token? = nil
        var isBarrier: Bool {
            token != nil || type == "playback.clear" || type == "voice.deactivated"
        }
    }
    private let targetName: String?
    private let exitAfterAcknowledgement: String?
    private var manager: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var controlCharacteristic: CBCharacteristic?
    private var statusCharacteristic: CBCharacteristic?
    private var microphoneCharacteristic: CBCharacteristic?
    private var speakerCharacteristic: CBCharacteristic?
    private var retryWorkItem: DispatchWorkItem?
    private var queuedMessages: [ControlWrite] = []
    private var activeMessage: ControlWrite?
    private var pendingChunks: [Data] = []
    private let speakerTransport = SpeakerTransport()
    private var speakerPacingWorkItem: DispatchWorkItem?
    private var speakerWatchdogWorkItem: DispatchWorkItem?
    private var readyToWrite = false
    private var writeInProgress = false
    private var controlWriteStartedAt = 0.0
    private var statusNotifying = false
    private var microphoneNotifying = false
    private var notificationBuffer = ""
    private let microphoneTransport = MicrophoneTransport()
    private var lastState = ""
    private var stopping = false

    init(targetName: String?, exitAfterAcknowledgement: String?) {
        self.targetName = targetName
        self.exitAfterAcknowledgement = exitAfterAcknowledgement
        super.init()
        manager = CBCentralManager(
            delegate: self,
            queue: DispatchQueue.main,
            options: [CBCentralManagerOptionShowPowerAlertKey: true]
        )
    }

    func receive(line: String) {
        let source = line.trimmingCharacters(in: .whitespacesAndNewlines)
        if let input = source.data(using: .utf8),
           let object = try? JSONSerialization.jsonObject(with: input),
           let dictionary = object as? [String: Any],
           let type = dictionary["type"] as? String {
            if type == "audio.delta" {
                guard let encoded = dictionary["audio"] as? String,
                      let audio = Data(base64Encoded: encoded),
                      !audio.isEmpty,
                      audio.count.isMultiple(of: 2) else {
                    report("rejected-audio")
                    return
                }
                switch speakerTransport.enqueueAudio(linearPCMToMuLaw(audio),
                                                       responseID: dictionary["responseId"] as? String) {
                case .accepted:
                    drainSpeakerWrites()
                case .stale:
                    break
                case .overflow:
                    failSpeakerStream("speaker-overrun")
                }
                return
            }
            if type == "audio.begin", let message = normalizedMessage(source) {
                if !speakerTransport.enqueueBegin(id: dictionary["responseId"] as? String ?? "ble-response",
                                                   message: message) {
                    report("rejected-audio-begin", repeatable: true)
                }
                drainSpeakerWrites()
                return
            }
            if type == "audio.done", let message = normalizedMessage(source) {
                if !speakerTransport.enqueueEnd(responseID: dictionary["responseId"] as? String,
                                                 message: message) {
                    report("rejected-audio-done", repeatable: true)
                }
                drainSpeakerWrites()
                return
            }
            if type == "playback.clear" || type == "voice.deactivated" {
                speakerTransport.clear()
                cancelSpeakerPacing()
                queuedMessages.removeAll { $0.token != nil || $0.type == "voice.state"
                    || $0.type == type }
                guard let message = normalizedMessage(source) else {
                    report("rejected-message")
                    return
                }
                queuedMessages.insert(ControlWrite(data: message, type: type), at: 0)
                beginWriteIfPossible()
                return
            }
            if let message = normalizedMessage(source) {
                if type == "voice.state" { queuedMessages.removeAll { $0.type == type } }
                queuedMessages.append(ControlWrite(data: message, type: type))
                beginWriteIfPossible()
                drainSpeakerWrites()
                return
            }
        }
        report("rejected-message")
    }

    func stop(exitCode: Int32? = nil) {
        guard !stopping else {
            return
        }
        stopping = true
        retryWorkItem?.cancel()
        speakerPacingWorkItem?.cancel()
        speakerPacingWorkItem = nil
        speakerWatchdogWorkItem?.cancel()
        speakerWatchdogWorkItem = nil
        manager.stopScan()
        if let peripheral {
            manager.cancelPeripheralConnection(peripheral)
        }
        if let exitCode {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                exit(exitCode)
            }
        }
    }

    private func report(_ state: String, repeatable: Bool = false) {
        if repeatable || state != lastState {
            lastState = state
            emit("STATE " + state)
        }
    }

    private func startScanning(after delay: TimeInterval = 0) {
        retryWorkItem?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self, !self.stopping, self.manager.state == .poweredOn else {
                return
            }
            self.peripheral = nil
            self.controlCharacteristic = nil
            self.statusCharacteristic = nil
            self.microphoneCharacteristic = nil
            self.speakerCharacteristic = nil
            self.readyToWrite = false
            self.writeInProgress = false
            self.statusNotifying = false
            self.microphoneNotifying = false
            self.pendingChunks.removeAll()
            self.speakerTransport.resetConnection()
            self.queuedMessages.removeAll { $0.token != nil || $0.type == "voice.state" }
            self.cancelSpeakerPacing()
            self.speakerWatchdogWorkItem?.cancel()
            self.speakerWatchdogWorkItem = nil
            self.notificationBuffer = ""
            self.microphoneTransport.reset()
            self.report("scanning")
            self.manager.scanForPeripherals(
                withServices: [serviceUUID],
                options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
            )
        }
        retryWorkItem = work
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
    }

    private func beginWriteIfPossible() {
        guard readyToWrite,
              !writeInProgress,
              activeMessage == nil,
              !queuedMessages.isEmpty,
              let peripheral,
              let controlCharacteristic else {
            return
        }
        let message = queuedMessages.removeFirst()
        activeMessage = message
        let maximumLength = max(
            20,
            peripheral.maximumWriteValueLength(for: .withResponse)
        )
        pendingChunks = chunks(of: message.data, maximumLength: maximumLength)
        guard !pendingChunks.isEmpty else {
            activeMessage = nil
            return
        }
        writeInProgress = true
        writeNextChunk(to: peripheral, characteristic: controlCharacteristic)
    }

    private func writeNextChunk(
        to peripheral: CBPeripheral,
        characteristic: CBCharacteristic
    ) {
        guard !pendingChunks.isEmpty else {
            let token = activeMessage?.token
            writeInProgress = false
            activeMessage = nil
            if let token {
                speakerTransport.controlCompleted(token, now: ProcessInfo.processInfo.systemUptime)
            }
            report("sent", repeatable: true)
            beginWriteIfPossible()
            drainSpeakerWrites()
            return
        }
        let chunk = pendingChunks.removeFirst()
        controlWriteStartedAt = ProcessInfo.processInfo.systemUptime
        peripheral.writeValue(chunk, for: characteristic, type: .withResponse)
    }

    private func writeFailed(_ error: Error) {
        writeInProgress = false
        pendingChunks.removeAll()
        if let activeMessage {
            queuedMessages.insert(activeMessage, at: 0)
            self.activeMessage = nil
        }
        report("write-error:" + oneLine(error.localizedDescription))
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            self?.beginWriteIfPossible()
            self?.drainSpeakerWrites()
        }
    }

    private func cancelSpeakerPacing() {
        speakerPacingWorkItem?.cancel()
        speakerPacingWorkItem = nil
    }

    private func scheduleSpeakerDrain(after delay: TimeInterval) {
        guard speakerPacingWorkItem == nil, speakerTransport.hasPendingResponse else {
            return
        }
        let work = DispatchWorkItem { [weak self] in
            guard let self else {
                return
            }
            self.speakerPacingWorkItem = nil
            self.drainSpeakerWrites()
        }
        speakerPacingWorkItem = work
        DispatchQueue.main.asyncAfter(
            deadline: .now() + max(0.004, delay),
            execute: work
        )
    }

    private func failSpeakerStream(_ reason: String) {
        speakerTransport.clear()
        cancelSpeakerPacing()
        queuedMessages.removeAll { $0.token != nil || $0.type == "voice.state"
            || $0.type == "status" || $0.type == "playback.clear" }
        if let status = normalizedMessage("{\"type\":\"status\"}") {
            queuedMessages.insert(ControlWrite(data: status, type: "status"), at: 0)
        }
        if let clear = normalizedMessage("{\"type\":\"playback.clear\",\"reason\":\"speaker-flow-failed\"}") {
            queuedMessages.insert(ControlWrite(data: clear, type: "playback.clear"), at: 0)
        }
        report(reason, repeatable: true)
        if writeInProgress,
           ProcessInfo.processInfo.systemUptime - controlWriteStartedAt >= 10,
           let peripheral {
            // An unacknowledged control write cannot safely be skipped midway
            // through a newline-framed message. Reconnect to clear that barrier.
            manager.cancelPeripheralConnection(peripheral)
            return
        }
        beginWriteIfPossible()
    }

    private func scheduleSpeakerWatchdog() {
        guard speakerWatchdogWorkItem == nil, readyToWrite,
              speakerTransport.hasPendingResponse else { return }
        let work = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.speakerWatchdogWorkItem = nil
            if self.speakerTransport.stalled(now: ProcessInfo.processInfo.systemUptime) {
                self.failSpeakerStream("speaker-flow-timeout")
            } else if self.speakerTransport.hasPendingResponse {
                self.scheduleSpeakerWatchdog()
            }
        }
        speakerWatchdogWorkItem = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 1, execute: work)
    }

    private func drainSpeakerWrites() {
        guard readyToWrite,
              let peripheral,
              let speakerCharacteristic else {
            return
        }
        scheduleSpeakerWatchdog()
        beginWriteIfPossible()
        // Ordinary status/UI controls may coexist with PCM. Stream boundaries
        // must finish their acknowledged control write before more audio.
        guard activeMessage?.isBarrier != true,
              !queuedMessages.contains(where: { $0.isBarrier }) else { return }
        let maximumLength = max(
            20,
            peripheral.maximumWriteValueLength(for: .withoutResponse)
        )
        while let action = speakerTransport.nextAction(
            maximumPacketBytes: maximumLength,
            canSendAudio: peripheral.canSendWriteWithoutResponse,
            now: ProcessInfo.processInfo.systemUptime) {
            switch action {
            case .audio(let packet):
                peripheral.writeValue(packet, for: speakerCharacteristic, type: .withoutResponse)
            case .control(let control):
                let type = control.token.kind == .begin ? "audio.begin" : "audio.done"
                queuedMessages.insert(ControlWrite(data: control.data, type: type,
                                                    token: control.token), at: 0)
                beginWriteIfPossible()
                return
            case .rejected(let reason):
                failSpeakerStream(reason)
                return
            }
        }
        if peripheral.canSendWriteWithoutResponse,
           let delay = speakerTransport.pacingDelay(maximumPacketBytes: maximumLength,
                                                   now: ProcessInfo.processInfo.systemUptime) {
            scheduleSpeakerDrain(after: delay)
        } else {
            cancelSpeakerPacing()
        }
    }

    private func handleDeviceLine(_ line: String) {
        emit("DEVICE " + line)
        guard let data = line.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let type = object["type"] as? String else { return }
        if type == "microphone.reset" {
            microphoneTransport.reset()
        } else if type == "status" {
            speakerTransport.setCapability(protocolUInt32(object["speakerFlow"]) == 1)
            drainSpeakerWrites()
        } else if type == "speaker.credit" {
            if let responseID = object["responseId"] as? String,
               let streamID = protocolUInt32(object["streamId"]), streamID != 0,
               let received = protocolUInt32(object["received"]),
               let limit = protocolUInt32(object["limit"]),
               let state = object["state"] as? String {
                if speakerTransport.receiveCredit(responseID: responseID, streamID: streamID,
                    received: received, limit: limit, state: state,
                    now: ProcessInfo.processInfo.systemUptime) == .invalid {
                    failSpeakerStream("speaker-credit-invalid")
                }
                drainSpeakerWrites()
            } else {
                report("speaker-credit-invalid")
            }
        } else if type == "playback.ended" || type == "playback.cancelled",
                  let responseID = object["responseId"] as? String {
            speakerTransport.receivePlaybackEnd(responseID: responseID,
                                                 cancelled: type == "playback.cancelled")
            drainSpeakerWrites()
        } else if type == "ack", object["ok"] as? Bool == false,
                  let request = object["request"] as? String,
                  (request == "audio.begin" || request == "audio.done"),
                  speakerTransport.hasPendingResponse {
            failSpeakerStream(request == "audio.begin" ? "speaker-begin-rejected" : "speaker-end-rejected")
        }
        guard let expected = exitAfterAcknowledgement, type == "ack",
              object["request"] as? String == expected else {
            return
        }
        stop(exitCode: object["ok"] as? Bool == true ? EXIT_SUCCESS : EXIT_FAILURE)
    }

    private func finishSubscriptionsIfReady(_ peripheral: CBPeripheral) {
        guard statusNotifying, microphoneNotifying, !readyToWrite else {
            return
        }
        readyToWrite = true
        report("audio-connected:" + (peripheral.name ?? "JUFF"))
        receive(line: "{\"type\":\"status\"}")
        beginWriteIfPossible()
        drainSpeakerWrites()
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            startScanning()
        case .poweredOff:
            report("powered-off")
        case .unauthorized:
            report("unauthorized")
        case .unsupported:
            report("unsupported")
        case .resetting:
            report("resetting")
        case .unknown:
            report("unknown")
        @unknown default:
            report("unknown-state")
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let advertisedName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        let discoveredName = advertisedName ?? peripheral.name
        if let targetName {
            guard discoveredName == targetName else {
                return
            }
        } else if let discoveredName {
            guard discoveredName.hasPrefix(deviceNamePrefix) else {
                return
            }
        }

        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        report("connecting:" + (discoveredName ?? "JUFF"))
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        report("discovering")
        peripheral.discoverServices([serviceUUID])
    }

    func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        report(error.map { "connect-error:" + oneLine($0.localizedDescription) }
            ?? "connect-error")
        startScanning(after: 2)
    }

    func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        guard !stopping else {
            return
        }
        readyToWrite = false
        speakerTransport.resetConnection()
        cancelSpeakerPacing()
        speakerWatchdogWorkItem?.cancel()
        speakerWatchdogWorkItem = nil
        queuedMessages.removeAll { $0.token != nil || $0.type == "voice.state" }
        notificationBuffer = ""
        if let activeMessage, activeMessage.token == nil {
            queuedMessages.insert(activeMessage, at: 0)
        }
        activeMessage = nil
        writeInProgress = false
        pendingChunks.removeAll()
        report(error.map { "disconnected:" + oneLine($0.localizedDescription) }
            ?? "disconnected")
        startScanning(after: 1)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            report("service-error:" + oneLine(error.localizedDescription))
            manager.cancelPeripheralConnection(peripheral)
            return
        }
        guard let service = peripheral.services?.first(where: { $0.uuid == serviceUUID }) else {
            report("service-missing")
            manager.cancelPeripheralConnection(peripheral)
            return
        }
        peripheral.discoverCharacteristics(
            [controlUUID, statusUUID, microphoneUUID, speakerUUID],
            for: service
        )
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        if let error {
            report("characteristic-error:" + oneLine(error.localizedDescription))
            manager.cancelPeripheralConnection(peripheral)
            return
        }
        controlCharacteristic = service.characteristics?.first {
            $0.uuid == controlUUID
        }
        statusCharacteristic = service.characteristics?.first {
            $0.uuid == statusUUID
        }
        microphoneCharacteristic = service.characteristics?.first {
            $0.uuid == microphoneUUID
        }
        speakerCharacteristic = service.characteristics?.first {
            $0.uuid == speakerUUID
        }
        guard controlCharacteristic != nil,
              let statusCharacteristic,
              let microphoneCharacteristic,
              speakerCharacteristic != nil else {
            report("characteristic-missing")
            manager.cancelPeripheralConnection(peripheral)
            return
        }
        report("subscribing")
        peripheral.setNotifyValue(true, for: statusCharacteristic)
        peripheral.setNotifyValue(true, for: microphoneCharacteristic)
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateNotificationStateFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            report("subscribe-error:" + oneLine(error.localizedDescription))
            manager.cancelPeripheralConnection(peripheral)
            return
        }
        if characteristic.uuid == statusUUID {
            statusNotifying = characteristic.isNotifying
        } else if characteristic.uuid == microphoneUUID {
            microphoneNotifying = characteristic.isNotifying
        }
        finishSubscriptionsIfReady(peripheral)
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didWriteValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            writeFailed(error)
            return
        }
        guard let controlCharacteristic else {
            writeInProgress = false
            return
        }
        writeNextChunk(to: peripheral, characteristic: controlCharacteristic)
    }

    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        drainSpeakerWrites()
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            report("notify-error:" + oneLine(error.localizedDescription))
            return
        }
        guard let value = characteristic.value else {
            return
        }
        if characteristic.uuid == microphoneUUID {
            for encoded in microphoneTransport.receive(value) {
                let pcm = muLawToLinearPCM(encoded)
                emit("MIC " + pcm.base64EncodedString())
            }
            return
        }
        guard characteristic.uuid == statusUUID else {
            return
        }
        notificationBuffer += String(decoding: value, as: UTF8.self)
        if notificationBuffer.count > 4096 {
            notificationBuffer = String(notificationBuffer.suffix(4096))
        }
        while let newline = notificationBuffer.firstIndex(of: "\n") {
            let line = String(notificationBuffer[..<newline])
                .replacingOccurrences(of: "\r", with: "")
                .trimmingCharacters(in: .whitespaces)
            notificationBuffer.removeSubrange(...newline)
            if !line.isEmpty {
                handleDeviceLine(line)
            }
        }
    }
}

private func argument(after name: String) -> String? {
    guard let index = CommandLine.arguments.firstIndex(of: name),
          CommandLine.arguments.indices.contains(index + 1) else {
        return nil
    }
    return CommandLine.arguments[index + 1]
}

if CommandLine.arguments.contains("--self-test") {
    exit(runSelfTest() ? EXIT_SUCCESS : EXIT_FAILURE)
}

private let noStdin = CommandLine.arguments.contains("--no-stdin")
private let transport = JuffCentral(
    targetName: argument(after: "--device"),
    exitAfterAcknowledgement: argument(after: "--exit-after-ack")
)

if !noStdin {
    DispatchQueue.global(qos: .utility).async {
        while let line = readLine(strippingNewline: true) {
            DispatchQueue.main.async {
                transport.receive(line: line)
            }
        }
        DispatchQueue.main.async {
            transport.stop(exitCode: EXIT_SUCCESS)
        }
    }
}
RunLoop.main.run()
