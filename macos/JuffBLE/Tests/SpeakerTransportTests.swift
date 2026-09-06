import Foundation

private func message(_ type: String, _ id: String) -> Data {
    var data = try! JSONSerialization.data(withJSONObject: ["type": type, "responseId": id])
    data.append(0x0a)
    return data
}

private func next(_ flow: SpeakerTransport, maximum: Int = 20, ready: Bool = true,
                  now: Double = 0) -> SpeakerTransport.Action? {
    flow.nextAction(maximumPacketBytes: maximum, canSendAudio: ready, now: now)
}

private func control(_ action: SpeakerTransport.Action?) -> SpeakerTransport.Control {
    guard case .control(let value) = action else { fatalError("Expected control") }
    return value
}

private func audio(_ action: SpeakerTransport.Action?) -> Data {
    guard case .audio(let value) = action else { fatalError("Expected audio") }
    return value
}

private func integer(_ data: Data, _ offset: Int) -> UInt32 {
    (0..<4).reduce(UInt32(0)) { $0 | (UInt32(data[offset + $1]) << ($1 * 8)) }
}

private func streamID(_ begin: SpeakerTransport.Control) -> UInt32 {
    let object = try! JSONSerialization.jsonObject(with: begin.data) as! [String: Any]
    precondition(object["flowControl"] as? Int == 1)
    return UInt32(object["streamId"] as! Int)
}

private func open(_ id: String = "A", bytes: Data = Data(), legacy: Bool = false)
    -> (SpeakerTransport, UInt32, SpeakerTransport.Control) {
    let flow = SpeakerTransport()
    flow.setCapability(!legacy)
    precondition(flow.enqueueBegin(id: id, message: message("audio.begin", id)))
    if !bytes.isEmpty { precondition(flow.enqueueAudio(bytes, responseID: id) == .accepted) }
    let begin = control(next(flow))
    let stream = legacy ? 0 : streamID(begin)
    flow.controlCompleted(begin.token, now: 0)
    return (flow, stream, begin)
}

private func credit(_ flow: SpeakerTransport, _ stream: UInt32, received: UInt32,
                    limit: UInt32, id: String = "A", state: String = "open", now: Double = 0)
    -> SpeakerTransport.CreditResult {
    flow.receiveCredit(responseID: id, streamID: stream, received: received,
                       limit: limit, state: state, now: now)
}

private func capabilityAndLegacy() {
    let flow = SpeakerTransport()
    precondition(flow.enqueueBegin(id: "A", message: message("audio.begin", "A")))
    let input = Data(repeating: 0xa1, count: 9000)
    precondition(flow.enqueueAudio(input, responseID: "A") == .accepted)
    precondition(next(flow) == nil) // Initial status is mandatory, including legacy detection.
    flow.setCapability(false)
    let begin = control(next(flow))
    let object = try! JSONSerialization.jsonObject(with: begin.data) as! [String: Any]
    precondition(object["flowControl"] == nil && object["streamId"] == nil)
    precondition(next(flow) == nil) // GATT BEGIN barrier.
    flow.controlCompleted(begin.token, now: 0)
    var sent = Data()
    while let action = next(flow, maximum: 1000) { sent.append(audio(action)) }
    precondition(sent.count == 7000 && sent == input.prefix(7000))
    precondition(flow.pacingDelay(maximumPacketBytes: 1000, now: 0)! > 0)
    precondition(credit(flow, 1, received: 0, limit: 100000) == .ignored)
    sent.append(audio(next(flow, maximum: 1000, now: 0.04)))
    precondition(sent.count == 8000) // Legacy pacing retained; bytes have no stream header.
}

private func windowAndBackpressure() {
    let input = Data(0..<25)
    let (flow, stream, _) = open(bytes: input)
    precondition(stream != 0 && next(flow) == nil)
    precondition(credit(flow, stream, received: 0, limit: 5, id: "old") == .ignored)
    precondition(credit(flow, stream + 1, received: 0, limit: 5) == .ignored)
    precondition(credit(flow, stream, received: 0, limit: 5) == .accepted)
    precondition(next(flow, maximum: 8) == nil)
    precondition(next(flow, ready: false) == nil)
    let first = audio(next(flow))
    precondition(first.count == 13 && integer(first, 0) == stream && integer(first, 4) == 0)
    precondition(first.dropFirst(8) == input.prefix(5))
    precondition(next(flow) == nil)
    precondition(credit(flow, stream, received: 0, limit: 5) == .accepted)
    precondition(next(flow) == nil) // Duplicate credits cannot release the same bytes twice.
    precondition(credit(flow, stream, received: 5, limit: 25) == .accepted)
    let second = audio(next(flow))
    precondition(second.count == 20 && integer(second, 4) == 5)
    precondition(second.dropFirst(8) == input.subdata(in: 5..<17))
    let third = audio(next(flow))
    precondition(integer(third, 4) == 17 && third.dropFirst(8) == input.suffix(8))
    precondition(flow.queuedBytes == 0 && next(flow) == nil)
    precondition(flow.pacingDelay(maximumPacketBytes: 20, now: 0) == nil)
    let (wide, wideStream, _) = open(bytes: Data(repeating: 7, count: 1000))
    precondition(credit(wide, wideStream, received: 0, limit: 1000) == .accepted)
    precondition(audio(next(wide, maximum: 512)).count == 244)
    precondition(integer(audio(next(wide, maximum: 512)), 4) == 236)
}

private func orderedResponsesAndEndFence() {
    let (flow, streamA, _) = open(bytes: Data(repeating: 0xa1, count: 12))
    precondition(flow.enqueueEnd(responseID: "A", message: message("audio.done", "A")))
    precondition(flow.enqueueBegin(id: "B", message: message("audio.begin", "B")))
    precondition(flow.enqueueAudio(Data(repeating: 0xb2, count: 12), responseID: "B") == .accepted)
    precondition(flow.enqueueEnd(responseID: "B", message: message("audio.done", "B")))
    precondition(next(flow) == nil) // B cannot bypass A, even before A's first credit.
    precondition(credit(flow, streamA, received: 0, limit: 12) == .accepted)
    precondition(audio(next(flow)).dropFirst(8) == Data(repeating: 0xa1, count: 12))
    precondition(flow.queuedBytes == 12 && next(flow) == nil) // Sender empty is not receiver ACK.
    precondition(credit(flow, streamA, received: 12, limit: 12) == .accepted)
    let endA = control(next(flow))
    let endObject = try! JSONSerialization.jsonObject(with: endA.data) as! [String: Any]
    precondition(endObject["streamId"] as? UInt32 == streamA)
    precondition(endObject["flowControl"] == nil && endA.data.last == 0x0a)
    precondition(endA.token.kind == .end && next(flow) == nil)
    flow.receivePlaybackEnd(responseID: "A", cancelled: false)
    precondition(next(flow) == nil) // A v1 terminal requires its stream fence.
    precondition(credit(flow, streamA, received: 12, limit: 12, state: "ended") == .accepted)
    precondition(next(flow) == nil) // Terminal may precede GATT ACK; finish that barrier first.
    flow.controlCompleted(endA.token, now: 0.1)
    let beginB = control(next(flow))
    let streamB = streamID(beginB)
    precondition(streamB > streamA)
    precondition(credit(flow, streamA, received: 12, limit: 12, state: "ended") == .ignored)
    flow.controlCompleted(beginB.token, now: 0.2)
    precondition(next(flow) == nil)
    precondition(credit(flow, streamB, received: 0, limit: 12, id: "B") == .accepted)
    precondition(audio(next(flow)).dropFirst(8) == Data(repeating: 0xb2, count: 12))
}

private func clearAndReconnectFences() {
    let (flow, oldStream, oldBegin) = open(bytes: Data(repeating: 0xa1, count: 12))
    precondition(credit(flow, oldStream, received: 0, limit: 12) == .accepted)
    let alreadySubmitted = audio(next(flow))
    flow.clear()
    precondition(flow.queuedBytes == 0 && !flow.hasPendingResponse)
    // Reuse the same response string to ensure matching only its name is insufficient.
    precondition(flow.enqueueBegin(id: "A", message: message("audio.begin", "A")))
    precondition(flow.enqueueAudio(Data(repeating: 0xb2, count: 12), responseID: "A") == .accepted)
    let begin = control(next(flow))
    let newStream = streamID(begin)
    precondition(newStream > oldStream && integer(alreadySubmitted, 0) != newStream)
    flow.controlCompleted(oldBegin.token, now: 0)
    precondition(credit(flow, oldStream, received: 0, limit: 1000) == .ignored)
    precondition(credit(flow, oldStream, received: 12, limit: 1000, state: "cancelled") == .ignored)
    precondition(next(flow) == nil)
    flow.controlCompleted(begin.token, now: 0)
    precondition(next(flow) == nil)
    precondition(credit(flow, newStream, received: 0, limit: 12) == .accepted)
    let current = audio(next(flow))
    precondition(integer(current, 0) == newStream && integer(current, 4) == 0)
    precondition(current.dropFirst(8) == Data(repeating: 0xb2, count: 12))
    flow.resetConnection()
    precondition(flow.enqueueBegin(id: "B", message: message("audio.begin", "B")))
    precondition(next(flow) == nil)
    precondition(credit(flow, newStream, received: 12, limit: 1000, state: "ended") == .ignored)
    flow.setCapability(true)
    precondition(streamID(control(next(flow))) > newStream)
}

private func invalidCreditsFailClosed() {
    for invalid in 0..<3 {
        let (flow, stream, _) = open(bytes: Data(repeating: 1, count: 100))
        precondition(credit(flow, stream, received: 0, limit: 12) == .accepted)
        _ = audio(next(flow))
        if invalid == 0 {
            precondition(credit(flow, stream, received: 13, limit: 100) == .invalid)
        } else if invalid == 1 {
            precondition(credit(flow, stream, received: 12, limit: 11) == .invalid)
        } else {
            precondition(credit(flow, stream, received: 0, limit: 10) == .invalid)
        }
        precondition(next(flow) == nil)
    }
    let (flow, stream, _) = open(bytes: Data(repeating: 1, count: 100))
    precondition(credit(flow, stream, received: 0, limit: 12) == .accepted)
    _ = audio(next(flow))
    precondition(credit(flow, stream, received: 12, limit: 24) == .accepted)
    precondition(credit(flow, stream, received: 0, limit: 12) == .ignored)
    precondition(integer(audio(next(flow)), 4) == 12)
}

private func timeoutAndLostEvents() {
    let (flow, stream, _) = open(bytes: Data(repeating: 1, count: 100))
    precondition(!flow.stalled(now: 9.9) && flow.stalled(now: 10))
    precondition(credit(flow, stream, received: 0, limit: 0, now: 10) == .accepted)
    for iteration in 1...100 {
        let now = 10 + Double(iteration) / 10
        precondition(credit(flow, stream, received: 0, limit: 0, now: now) == .accepted)
    }
    precondition(flow.stalled(now: 20)) // Repeated zero-credit notices are not progress.
    flow.clear()
    precondition(!flow.stalled(now: 30) && next(flow) == nil)

    let (empty, emptyStream, _) = open()
    precondition(empty.enqueueEnd(responseID: "A", message: message("audio.done", "A")))
    precondition(next(empty) == nil)
    precondition(credit(empty, emptyStream, received: 0, limit: 0) == .accepted)
    let end = control(next(empty))
    empty.controlCompleted(end.token, now: 0)
    precondition(empty.hasPendingResponse)
    // No generic playback event is required: repeated terminal credits recover its loss.
    precondition(credit(empty, emptyStream, received: 0, limit: 0, state: "ended") == .accepted)
    precondition(!empty.hasPendingResponse)
}

private func queueLimitsAndLegacyEnd() {
    let flow = SpeakerTransport()
    precondition(flow.enqueueBegin(id: "A", message: message("audio.begin", "A")))
    precondition(flow.enqueueBegin(id: "A", message: message("audio.begin", "A")))
    precondition(flow.enqueueAudio(Data(repeating: 0, count: SpeakerTransport.maximumQueuedBytes),
                                   responseID: "A") == .accepted)
    precondition(flow.enqueueAudio(Data([1]), responseID: "A") == .overflow)
    precondition(flow.enqueueAudio(Data([1]), responseID: "stale") == .stale)
    precondition(flow.queuedBytes == SpeakerTransport.maximumQueuedBytes)
    flow.clear()
    precondition(flow.queuedBytes == 0)
    for index in 0..<32 {
        precondition(flow.enqueueBegin(id: "queued-\(index)",
                                       message: message("audio.begin", "queued-\(index)")))
    }
    precondition(!flow.enqueueBegin(id: "overflow", message: message("audio.begin", "overflow")))
    flow.clear()

    let (legacy, _, _) = open(bytes: Data([1, 2, 3]), legacy: true)
    precondition(legacy.enqueueEnd(responseID: "A", message: message("audio.done", "A")))
    precondition(legacy.enqueueBegin(id: "B", message: message("audio.begin", "B")))
    precondition(audio(next(legacy)) == Data([1, 2, 3]))
    let end = control(next(legacy))
    let legacyEnd = try! JSONSerialization.jsonObject(with: end.data) as! [String: Any]
    precondition(legacyEnd["streamId"] == nil && legacyEnd["flowControl"] == nil)
    legacy.controlCompleted(end.token, now: 0)
    precondition(next(legacy) == nil)
    legacy.receivePlaybackEnd(responseID: "different", cancelled: false)
    precondition(next(legacy) == nil)
    legacy.receivePlaybackEnd(responseID: "A", cancelled: false)
    precondition(control(next(legacy)).token.kind == .begin)
}

private func staleVersusOverflow() {
    let (flow, stream, _) = open(bytes: Data(repeating: 1, count: SpeakerTransport.maximumQueuedBytes))
    precondition(credit(flow, stream, received: 0, limit: 12) == .accepted)
    // A completely full current queue still ignores unmatched old input.
    precondition(flow.enqueueAudio(Data([9]), responseID: "old") == .stale)
    precondition(flow.queuedBytes == SpeakerTransport.maximumQueuedBytes)
    precondition(audio(next(flow)).dropFirst(8) == Data(repeating: 1, count: 12))
    precondition(flow.enqueueAudio(Data(repeating: 2, count: 12), responseID: "A") == .accepted)
    // A matched overflow is a distinct fatal result; the controller invokes
    // clear/CLEAR/status rather than allowing a later END to conceal a hole.
    precondition(flow.enqueueAudio(Data([2]), responseID: "A") == .overflow)
    flow.clear()
    precondition(!flow.hasPendingResponse && flow.queuedBytes == 0)
    precondition(!flow.enqueueEnd(responseID: "A", message: message("audio.done", "A")))
    precondition(next(flow) == nil)
    precondition(credit(flow, stream, received: 12, limit: 1000) == .ignored)
    precondition(flow.enqueueBegin(id: "B", message: message("audio.begin", "B")))
    precondition(flow.enqueueAudio(Data([3]), responseID: "B") == .accepted)
    precondition(flow.enqueueAudio(Data([9]), responseID: "A") == .stale)
    precondition(flow.queuedBytes == 1 && control(next(flow)).token.kind == .begin)
}

private func singleLinkPacketAudio() {
    // A 256-byte ATT MTU permits a 253-byte GATT value, but that value needs
    // two 251-byte link packets after the L2CAP and ATT headers are included.
    for maximum in [20, 182, 244, 253, 512] {
        let input = Data((0..<2003).map { UInt8(truncatingIfNeeded: $0) })
        let (flow, stream, _) = open(bytes: input)
        precondition(credit(flow, stream, received: 0, limit: 997) == .accepted)
        var rebuilt = Data()
        for limit in [997, input.count] {
            while let action = next(flow, maximum: maximum) {
                let packet = audio(action)
                precondition(packet.count <= maximum && packet.count + 4 + 3 <= 251)
                precondition(integer(packet, 0) == stream)
                precondition(integer(packet, 4) == UInt32(rebuilt.count))
                rebuilt.append(packet.dropFirst(8))
            }
            precondition(rebuilt.count == limit)
            precondition(credit(flow, stream, received: UInt32(limit),
                                limit: UInt32(input.count)) == .accepted)
        }
        precondition(rebuilt == input && flow.queuedBytes == 0)
        precondition(flow.pacingDelay(maximumPacketBytes: maximum, now: 0) == nil)
    }
}

@main
private enum SpeakerTransportTests {
    static func main() {
        precondition(CommandLine.arguments.count == 2)
        switch CommandLine.arguments[1] {
        case "legacy": capabilityAndLegacy()
        case "window": windowAndBackpressure()
        case "ordering": orderedResponsesAndEndFence()
        case "clear": clearAndReconnectFences()
        case "invalid": invalidCreditsFailClosed()
        case "timeout": timeoutAndLostEvents()
        case "limits": queueLimitsAndLegacyEnd()
        case "overflow": staleVersusOverflow()
        case "link-packet": singleLinkPacketAudio()
        default: fatalError("Unknown scenario")
        }
        print("Speaker transport regression passed")
    }
}
