// send_midi — emit a MIDI NoteOn/NoteOff to IAC Driver Bus 1 (a
// persistent system MIDI bus). User enables IAC Driver Bus 1 as an
// input in DS once; we send to it from any process.
//
//   swift send_midi.swift on  <note> <vel>
//   swift send_midi.swift off <note>

import Foundation
import CoreMIDI

let args = CommandLine.arguments
guard args.count >= 3 else {
    FileHandle.standardError.write("usage: send_midi.swift {on|off} <note> [vel]\n".data(using: .utf8)!)
    exit(1)
}
let action = args[1]
let note   = UInt8(args[2])!
let vel    = args.count > 3 ? UInt8(args[3])! : 100

var client = MIDIClientRef()
MIDIClientCreate("ds_render-client" as CFString, nil, nil, &client)
var port = MIDIPortRef()
MIDIOutputPortCreate(client, "ds_render-out" as CFString, &port)

// Find IAC Driver Bus 1 by name.
var iac: MIDIEndpointRef = 0
for i in 0..<MIDIGetNumberOfDestinations() {
    let ep = MIDIGetDestination(i)
    var name: Unmanaged<CFString>?
    MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &name)
    if let n = name?.takeRetainedValue() as String?, n == "IAC Driver Bus 1" {
        iac = ep
        break
    }
}
guard iac != 0 else {
    FileHandle.standardError.write("IAC Driver Bus 1 not found — enable it in Audio MIDI Setup\n".data(using: .utf8)!)
    exit(2)
}

var packetList = MIDIPacketList()
let packet = MIDIPacketListInit(&packetList)
let bytes: [UInt8] = action == "on" ? [0x90, note, vel] : [0x80, note, 0]
_ = MIDIPacketListAdd(&packetList,
                      MemoryLayout<MIDIPacketList>.size,
                      packet, 0, bytes.count, bytes)
MIDISend(port, iac, &packetList)
Thread.sleep(forTimeInterval: 0.05)
MIDIPortDispose(port)
MIDIClientDispose(client)
