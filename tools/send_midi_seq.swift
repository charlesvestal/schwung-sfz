// send_midi_seq — send a sequence of MIDI events with relative
// delays. Each event: "<delay_ms> <on|off> <note> [<vel>]"
//
//   swift send_midi_seq.swift <<EOF
//   0    on 60 100
//   0    on 64 100
//   0    on 67 100
//   2000 off 60
//   2000 off 64
//   2000 off 67
//   EOF

import Foundation
import CoreMIDI

var client = MIDIClientRef()
MIDIClientCreate("ds_render-seq-client" as CFString, nil, nil, &client)
var port = MIDIPortRef()
MIDIOutputPortCreate(client, "ds_render-seq-out" as CFString, &port)

var iac: MIDIEndpointRef = 0
for i in 0..<MIDIGetNumberOfDestinations() {
    let ep = MIDIGetDestination(i)
    var name: Unmanaged<CFString>?
    MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name)
    if let n = name?.takeRetainedValue() as String?, n == "IAC Driver Bus 1" {
        iac = ep; break
    }
}
guard iac != 0 else {
    FileHandle.standardError.write("IAC Driver Bus 1 not found\n".data(using: .utf8)!)
    exit(2)
}

func send(_ status: UInt8, _ d1: UInt8, _ d2: UInt8) {
    var pl = MIDIPacketList()
    let pkt = MIDIPacketListInit(&pl)
    let bytes: [UInt8] = [status, d1, d2]
    _ = MIDIPacketListAdd(&pl, MemoryLayout<MIDIPacketList>.size, pkt, 0,
                          bytes.count, bytes)
    MIDISend(port, iac, &pl)
}

while let line = readLine() {
    let parts = line.split(separator: " ", omittingEmptySubsequences: true)
    guard parts.count >= 3 else { continue }
    let dMs = Int(parts[0]) ?? 0
    let action = String(parts[1])
    let note = UInt8(parts[2]) ?? 60
    let vel  = parts.count > 3 ? (UInt8(parts[3]) ?? 100) : 100
    if dMs > 0 { Thread.sleep(forTimeInterval: Double(dMs) / 1000.0) }
    if action == "on"  { send(0x90, note, vel) }
    else               { send(0x80, note, 0)   }
}

Thread.sleep(forTimeInterval: 0.1)
MIDIPortDispose(port)
MIDIClientDispose(client)
