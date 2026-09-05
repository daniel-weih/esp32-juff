import Foundation

// CoreBluetooth-independent response ordering and receive-window accounting.
// All methods are called on the central's serial queue.
final class SpeakerTransport {
    static let maximumQueuedBytes = 2 * 1024 * 1024
    static let legacyBytesPerSecond = 25_200.0
    static let legacyBurstBytes = 7_200.0
    static let headerBytes = 8

    enum ControlKind { case begin, end }
    struct Token: Equatable {
        let serial: UInt64
        let kind: ControlKind
    }
    struct Control {
        let data: Data
        let token: Token
    }
    enum Action {
        case control(Control)
        case audio(Data)
        case rejected(String)
    }
    enum CreditResult { case accepted, ignored, invalid }
    enum AudioEnqueueResult { case accepted, stale, overflow }
    private enum Phase { case waitingBegin, writingBegin, streaming, writingEnd, awaitingEnd }
    private final class Response {
        let serial: UInt64
        let id: String
        let begin: Data
        var phase = Phase.waitingBegin
        var chunks: [Data] = []
        var offset = 0
        var queuedBytes = 0
        var acceptedBytes: UInt64 = 0
        var end: Data?
        var flow = false
        var streamID: UInt32 = 0
        var sent: UInt32 = 0
        var received: UInt32 = 0
        var limit: UInt32 = 0
        var haveCredit = false
        var invalidCredit = false
        var terminal = false
        var budget = SpeakerTransport.legacyBurstBytes
        var budgetAt = 0.0
        var progressAt: Double?

        init(serial: UInt64, id: String, begin: Data) {
            self.serial = serial
            self.id = id
            self.begin = begin
        }
    }

    private var responses: [Response] = []
    private var nextSerial: UInt64 = 1
    private var nextStreamID: UInt32 = 1
    private var capability: Bool?
    private(set) var queuedBytes = 0

    var hasPendingResponse: Bool { !responses.isEmpty }

    func setCapability(_ supported: Bool) { capability = supported }

    func clear() {
        responses.removeAll(keepingCapacity: true)
        queuedBytes = 0
    }

    func resetConnection() {
        clear()
        capability = nil
        // Keep stream IDs distinct across reconnects in this helper's lifetime.
    }

    @discardableResult
    func enqueueBegin(id: String, message: Data) -> Bool {
        guard !id.isEmpty else { return false }
        // Some gateways send BEGIN and also synthesize it from the first delta.
        if responses.contains(where: { $0.id == id }) { return true }
        guard responses.count < 32 else { return false }
        responses.append(Response(serial: nextSerial, id: id, begin: message))
        nextSerial &+= 1
        return true
    }

    @discardableResult
    func enqueueAudio(_ data: Data, responseID: String?) -> AudioEnqueueResult {
        // Match first: an old delta must never abort a different, full stream.
        guard !data.isEmpty,
              let response = responseID.flatMap({ id in responses.first { $0.id == id } })
                ?? (responseID == nil ? responses.last : nil),
              response.end == nil, !response.terminal else {
            return .stale
        }
        guard data.count <= Self.maximumQueuedBytes - queuedBytes,
              response.acceptedBytes + UInt64(data.count) <= UInt64(UInt32.max) else {
            return .overflow
        }
        response.chunks.append(data)
        response.queuedBytes += data.count
        response.acceptedBytes += UInt64(data.count)
        queuedBytes += data.count
        return .accepted
    }

    @discardableResult
    func enqueueEnd(responseID: String?, message: Data) -> Bool {
        guard let response = responseID.flatMap({ id in responses.first { $0.id == id } })
                ?? (responseID == nil ? responses.last : nil),
              !response.terminal else { return false }
        if response.end == nil { response.end = message }
        return true
    }

    private func controlMessage(_ response: Response, source: Data, begin: Bool) -> Data? {
        guard var object = (try? JSONSerialization.jsonObject(with: source))
                as? [String: Any] else { return nil }
        object.removeValue(forKey: "flowControl")
        if response.flow {
            if begin { object["flowControl"] = 1 }
            object["streamId"] = response.streamID
        } else {
            object.removeValue(forKey: "streamId")
        }
        guard var data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]),
              data.count <= 512 else { return nil }
        data.append(0x0a)
        return data
    }

    private func updateBudget(_ response: Response, now: Double) {
        response.budget = min(Self.legacyBurstBytes,
            response.budget + max(0, now - response.budgetAt) * Self.legacyBytesPerSecond)
        response.budgetAt = now
    }

    func nextAction(maximumPacketBytes: Int, canSendAudio: Bool, now: Double) -> Action? {
        guard let response = responses.first, !response.terminal else { return nil }
        if response.progressAt == nil { response.progressAt = now }
        if response.phase == .waitingBegin {
            guard let capability else { return nil } // Wait for the initial STATUS.
            response.flow = capability
            if capability {
                guard nextStreamID != 0 else {
                    removeFirst()
                    return .rejected("speaker-stream-id-exhausted")
                }
                response.streamID = nextStreamID
                nextStreamID = nextStreamID == UInt32.max ? 0 : nextStreamID + 1
            }
            guard let message = controlMessage(response, source: response.begin, begin: true) else {
                removeFirst()
                return .rejected("speaker-begin-too-large")
            }
            response.budgetAt = now
            response.phase = .writingBegin
            return .control(Control(data: message,
                                    token: Token(serial: response.serial, kind: .begin)))
        }
        guard response.phase == .streaming, !response.invalidCredit else { return nil }
        if let chunk = response.chunks.first {
            guard canSendAudio else { return nil }
            let header = response.flow ? Self.headerBytes : 0
            guard maximumPacketBytes > header else { return nil }
            var count = min(maximumPacketBytes - header, chunk.count - response.offset)
            if response.flow {
                guard response.haveCredit, response.limit > response.sent else { return nil }
                count = min(count, Int(response.limit - response.sent))
            } else {
                updateBudget(response, now: now)
                guard response.budget >= Double(count) else { return nil }
            }
            var packet = Data()
            if response.flow {
                for value in [response.streamID, response.sent] {
                    for shift in stride(from: 0, to: 32, by: 8) {
                        packet.append(UInt8(truncatingIfNeeded: value >> shift))
                    }
                }
            }
            packet.append(chunk.subdata(in: response.offset..<(response.offset + count)))
            response.offset += count
            response.sent += UInt32(count)
            response.progressAt = now
            response.queuedBytes -= count
            queuedBytes -= count
            if !response.flow { response.budget -= Double(count) }
            if response.offset == chunk.count {
                response.chunks.removeFirst()
                response.offset = 0
            }
            return .audio(packet)
        }
        if let end = response.end {
            guard !response.flow || (response.haveCredit && response.received == response.sent) else {
                return nil
            }
            guard let message = controlMessage(response, source: end, begin: false) else {
                removeFirst()
                return .rejected("speaker-end-too-large")
            }
            response.phase = .writingEnd
            return .control(Control(data: message,
                                    token: Token(serial: response.serial, kind: .end)))
        }
        return nil
    }

    // Only legacy firmware uses a time budget. A v1 stream wakes on device
    // credits or CoreBluetooth readiness, so no fixed-rate timer throttles it.
    func pacingDelay(maximumPacketBytes: Int, now: Double) -> Double? {
        guard let response = responses.first, response.phase == .streaming,
              !response.flow, let chunk = response.chunks.first else { return nil }
        updateBudget(response, now: now)
        let count = min(maximumPacketBytes, chunk.count - response.offset)
        guard count > 0, response.budget < Double(count) else { return nil }
        return max(0.004, (Double(count) - response.budget) / Self.legacyBytesPerSecond)
    }

    func controlCompleted(_ token: Token, now: Double) {
        guard let response = responses.first, response.serial == token.serial else { return }
        switch token.kind {
        case .begin:
            guard response.phase == .writingBegin else { return }
            response.phase = .streaming
        case .end:
            guard response.phase == .writingEnd else { return }
            response.phase = .awaitingEnd
        }
        response.progressAt = now
        if response.terminal || (!response.flow && token.kind == .end && response.sent == 0) {
            removeFirst()
        }
    }

    @discardableResult
    func receiveCredit(responseID: String, streamID: UInt32, received: UInt32,
                       limit: UInt32, state: String, now: Double) -> CreditResult {
        guard let response = responses.first, response.flow,
              response.phase != .waitingBegin,
              response.id == responseID, response.streamID == streamID else { return .ignored }
        guard !response.invalidCredit else { return .invalid }
        guard received >= response.received else { return .ignored }
        guard received <= response.sent, limit >= received,
              !response.haveCredit || limit >= response.limit,
              ["open", "draining", "ended", "cancelled"].contains(state) else {
            response.invalidCredit = true
            return .invalid
        }
        if state == "ended", response.phase != .writingEnd && response.phase != .awaitingEnd {
            response.invalidCredit = true
            return .invalid
        }
        if !response.haveCredit || received > response.received || limit > response.limit {
            response.progressAt = now
        }
        response.haveCredit = true
        response.received = received
        response.limit = limit
        if state == "ended" || state == "cancelled" {
            response.terminal = true
            if response.phase != .writingBegin && response.phase != .writingEnd { removeFirst() }
        }
        return .accepted
    }

    func receivePlaybackEnd(responseID: String, cancelled: Bool) {
        guard let response = responses.first, !response.flow, response.id == responseID else { return }
        // V1 uses a terminal credit, whose stream ID also rejects late events
        // after CLEAR and a new BEGIN with the same response ID.
        guard cancelled || response.phase == .writingEnd || response.phase == .awaitingEnd else { return }
        response.terminal = true
        if response.phase != .writingBegin && response.phase != .writingEnd { removeFirst() }
    }

    func stalled(now: Double) -> Bool {
        guard let response = responses.first else { return false }
        guard let progress = response.progressAt else {
            response.progressAt = now
            return false
        }
        return now - progress >= 10
    }

    private func removeFirst() {
        guard let response = responses.first else { return }
        queuedBytes -= response.queuedBytes
        responses.removeFirst()
    }
}
