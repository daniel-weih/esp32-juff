import Foundation

// A reset is queued by the device on this connection before PCM from a new
// capture generation or after an incomplete frame. Keep it in the same serial
// delegate path as the PCM, so no old prefix can enter the next uploaded frame.
final class MicrophoneTransport {
    static let frameBytes = 1600
    private var pending = Data()

    func reset() { pending.removeAll(keepingCapacity: true) }

    func receive(_ data: Data) -> [Data] {
        pending.append(data)
        var frames: [Data] = []
        while pending.count >= Self.frameBytes {
            frames.append(pending.subdata(in: 0..<Self.frameBytes))
            pending.removeSubrange(0..<Self.frameBytes)
        }
        return frames
    }
}
