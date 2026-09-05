import Foundation

@main
private enum MicrophoneTransportTests {
    static func main() {
        let transport = MicrophoneTransport()
        if CommandLine.arguments.last == "wire-trace" {
            let wire = FileHandle.standardInput.readDataToEndOfFile()
            let text = String(data: wire, encoding: .utf8)!
            var frames: [Data] = []
            var boundaries = 0
            for line in text.split(separator: "\n") {
                let event = try! JSONSerialization.jsonObject(with: Data(line.utf8)) as! [String: Any]
                switch event["type"] as! String {
                case "microphone.reset":
                    transport.reset()
                    boundaries += 1
                case "microphone.data":
                    let hex = Array((event["hex"] as! String).utf8)
                    let bytes = stride(from: 0, to: hex.count, by: 2).map {
                        UInt8(String(bytes: hex[$0..<$0 + 2], encoding: .utf8)!, radix: 16)!
                    }
                    frames.append(contentsOf: transport.receive(Data(bytes)))
                default: fatalError("Unexpected wire event")
                }
            }
            // The production C sender accepted 253 old bytes before cancellation
            // and one full fresh frame after reset. No old prefix may survive.
            precondition(boundaries == 2 && frames.count == 1)
            precondition(frames[0] == Data(repeating: 0x22, count: 1600))
            precondition(transport.receive(Data(repeating: 0x33, count: 1600))
                         == [Data(repeating: 0x33, count: 1600)])
        } else {
            // Older firmware has no reset events: arbitrary ATT fragment sizes
            // must still concatenate into the same fixed 100 ms mu-law frames.
            let expected = Data((0..<4800).map { UInt8(truncatingIfNeeded: $0 * 17) })
            for chunk in [20, 182, 253] {
                transport.reset()
                var received = Data()
                for offset in stride(from: 0, to: expected.count, by: chunk) {
                    for frame in transport.receive(expected.subdata(in: offset..<min(offset + chunk, expected.count))) {
                        precondition(frame.count == 1600)
                        received.append(frame)
                    }
                }
                precondition(received == expected)
            }
            precondition(transport.receive(Data(repeating: 1, count: 253)).isEmpty)
            transport.reset() // Reconnect also discards an incomplete frame.
            precondition(transport.receive(Data(repeating: 2, count: 1600))
                         == [Data(repeating: 2, count: 1600)])
        }
        print("Microphone transport regression passed")
    }
}
