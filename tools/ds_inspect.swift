// Print DS AU param tree + fullState keys for diagnostic purposes.
import Foundation
import AVFoundation

let desc = AudioComponentDescription(
    componentType: kAudioUnitType_MusicDevice,
    componentSubType: 0x44736D70, componentManufacturer: 0x446C6479,
    componentFlags: 0, componentFlagsMask: 0)

let sem = DispatchSemaphore(value: 0)
var u: AVAudioUnit?
AVAudioUnit.instantiate(with: desc, options: []) { unit, _ in
    u = unit
    sem.signal()
}
sem.wait()
guard let ds = u else { print("instantiate failed"); exit(1) }

print("--- parameter tree ---")
if let tree = ds.auAudioUnit.parameterTree {
    for p in tree.allParameters {
        print("\(p.address)  \(p.identifier)  [\(p.minValue) .. \(p.maxValue)]  display='\(p.displayName)'")
    }
} else {
    print("(no parameter tree)")
}

print("\n--- factory presets ---")
if let presets = ds.auAudioUnit.factoryPresets {
    for p in presets { print("\(p.number)  \(p.name)") }
} else {
    print("(none)")
}

print("\n--- fullState keys (initial) ---")
if let st = ds.auAudioUnit.fullState {
    for (k, v) in st {
        let vs = String(describing: v)
        let snippet = vs.count > 80 ? String(vs.prefix(80)) + "..." : vs
        print("  \(k): \(snippet)")
    }
} else {
    print("(no fullState)")
}
