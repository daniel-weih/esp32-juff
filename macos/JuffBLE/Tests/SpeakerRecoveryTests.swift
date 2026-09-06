import Foundation

private let maximumMessageBytes = 512
private let allowedControlTypes: Set<String> = ["playback.clear", "status"]
// PRODUCTION_NORMALIZER

private final class RecoveryCentral {
    private struct ControlWrite {
        let data: Data
        let type: String
        var token: SpeakerTransport.Token? = nil
    }
    private final class Manager {
        func cancelPeripheralConnection(_ peripheral: Int) { fatalError("Unexpected reconnect") }
    }
    private let manager = Manager()
    private let peripheral: Int? = nil
    private let writeInProgress = false
    private let controlWriteStartedAt = 0.0
    private let conversationGeneration: UInt32?
    private let speakerTransport = SpeakerTransport()
    private var queuedMessages: [ControlWrite] = []
    private var pacingCancelled = false
    private var writesStarted = false
    private var activityUpdated = false
    private var reportedReason = ""

    init(generation: UInt32?) { conversationGeneration = generation }
    private func updateSpeakerActivity() { activityUpdated = true }
    private func cancelSpeakerPacing() { pacingCancelled = true }
    private func beginWriteIfPossible() { writesStarted = true }
    private func report(_ reason: String, repeatable: Bool) { reportedReason = reason }

    // PRODUCTION_FAILURE_HANDLER

    func recover(_ reason: String) -> Data {
        let begin = Data("{\"type\":\"audio.begin\",\"responseId\":\"test\"}".utf8)
        precondition(speakerTransport.enqueueBegin(id: "test", message: begin))
        precondition(speakerTransport.enqueueAudio(Data([0xff]), responseID: "test") == .accepted)
        queuedMessages = ["voice.state", "status", "playback.clear", "command"].map {
            ControlWrite(data: Data(), type: $0)
        }
        failSpeakerStream(reason)
        precondition(!speakerTransport.hasPendingResponse && speakerTransport.queuedBytes == 0)
        precondition(pacingCancelled && writesStarted && activityUpdated && reportedReason == reason)
        precondition(queuedMessages.map(\.type) == ["playback.clear", "status", "command"])
        precondition(queuedMessages[0].data.last == 0x0a)
        return queuedMessages[0].data
    }
}

@main
enum SpeakerRecoveryTests {
    static func main() {
        precondition(CommandLine.arguments.count == 3)
        let central = RecoveryCentral(generation: UInt32(CommandLine.arguments[1]))
        FileHandle.standardOutput.write(central.recover(CommandLine.arguments[2]))
    }
}
