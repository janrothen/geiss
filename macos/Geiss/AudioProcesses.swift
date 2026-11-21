import CoreAudio
import Foundation

enum AudioProcesses {
    /// Whether a process other than Geiss is sending audio to an output device. The tap's own
    /// aggregate device is input, so it does not count.
    static func otherProcessPlaysSound() -> Bool {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyProcessObjectList,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        let system = AudioObjectID(kAudioObjectSystemObject)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(system, &address, 0, nil, &size) == noErr, size > 0 else {
            return false
        }
        var processes = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(system, &address, 0, nil, &size, &processes) == noErr else {
            return false
        }
        let ownPID = ProcessInfo.processInfo.processIdentifier
        for process in processes.prefix(Int(size) / MemoryLayout<AudioObjectID>.size) {
            var pid: pid_t = 0
            var pidSize = UInt32(MemoryLayout<pid_t>.size)
            address.mSelector = kAudioProcessPropertyPID
            guard AudioObjectGetPropertyData(process, &address, 0, nil, &pidSize, &pid) == noErr,
                  pid != ownPID
            else {
                continue
            }
            var running: UInt32 = 0
            var runningSize = UInt32(MemoryLayout<UInt32>.size)
            address.mSelector = kAudioProcessPropertyIsRunningOutput
            if AudioObjectGetPropertyData(process, &address, 0, nil, &runningSize, &running) == noErr, running != 0 {
                return true
            }
        }
        return false
    }
}
