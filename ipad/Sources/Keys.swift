// USB HID keyboard usage (UIKeyboardHIDUsage, US layout positions) → Linux
// evdev key code, as Linux's own HID driver maps them. The whole hardware
// keyboard goes to the host, Command as Meta (Super), except the shortcuts
// iPadOS keeps for itself (Cmd+Tab, Cmd+H, Cmd+Space, Globe ones).

let evdevKeys: [Int: UInt16] = {
    // From usage 0x04 (A) to 0x67 (keypad =), in order.
    let main: [UInt16] = [
        30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, // a-z
        2, 3, 4, 5, 6, 7, 8, 9, 10, 11,                       // 1-0
        28, 1, 14, 15, 57, 12, 13, 26, 27, 43, 43, 39, 40, 41, 51, 52, 53, 58, // enter ... caps lock
        59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88,       // F1-F12
        99, 70, 119, 110, 102, 104, 111, 107, 109, 106, 105, 108, 103, // print screen ... up
        69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71, 72, 73, 82, 83, // keypad
        86, 127, 116, 117,                                    // non-US \, application, power, keypad =
    ]
    var m: [Int: UInt16] = [:]
    for (i, k) in main.enumerated() { m[0x04 + i] = k }
    for (i, k) in [183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194].enumerated() { m[0x68 + i] = UInt16(k) } // F13-F24
    for (u, k) in [(0x7F, 113), (0x80, 115), (0x81, 114), (0x85, 121), (0x87, 89), (0x88, 93), (0x89, 124), (0x8A, 92),
                   (0x8B, 94), (0x8C, 95), (0x90, 122), (0x91, 123)] { m[u] = UInt16(k) } // mute, volume, international
    for (i, k) in [29, 42, 56, 125, 97, 54, 100, 126].enumerated() { m[0xE0 + i] = UInt16(k) } // left/right ctrl, shift, alt, meta
    return m
}()
