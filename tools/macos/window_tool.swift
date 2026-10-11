// window_tool: helpers for tools/macos/play_route.py (native arm64, never translated).
//   window_tool window <pid>   prints the CGWindowID of the largest on-screen layer-0 window
//                              owned by <pid>; exits 2 when there is none.
//   window_tool diff <a> <b>   prints the fraction of changed pixels (largest channel delta > 24).
//   window_tool stats <png>    prints JSON image statistics: width, height, the fraction of
//                              pixels with any RGB channel above 16, and the mean luma (0..255).
import CoreGraphics
import Foundation
import ImageIO

func fail(_ message: String, _ code: Int32) -> Never {
    FileHandle.standardError.write((message + "\n").data(using: .utf8)!)
    exit(code)
}

let args = CommandLine.arguments
guard args.count >= 3 else { fail("usage: window_tool window <pid> | stats <png> | diff <a> <b>", 64) }

switch args[1] {
case "window":
    guard let pid = Int(args[2]) else { fail("window: pid must be an integer", 64) }
    if !CGPreflightScreenCaptureAccess() { fail("window: Screen Recording permission is not granted", 3) }
    let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID)
        as? [[String: Any]] ?? []
    var best: (id: Int, area: Double)? = nil
    for w in list {
        guard (w[kCGWindowOwnerPID as String] as? Int) == pid, (w[kCGWindowLayer as String] as? Int) == 0,
              let id = w[kCGWindowNumber as String] as? Int,
              let b = w[kCGWindowBounds as String] as? [String: Double] else { continue }
        let area = (b["Width"] ?? 0) * (b["Height"] ?? 0)
        if best == nil || area > best!.area { best = (id, area) }
    }
    guard let found = best else { fail("window: no on-screen window for pid \(pid)", 2) }
    print(found.id)
case "stats":
    let url = URL(fileURLWithPath: args[2])
    guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
          let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { fail("stats: cannot read \(args[2])", 1) }
    let width = image.width, height = image.height
    var pixels = [UInt8](repeating: 0, count: width * height * 4)
    let drawn = pixels.withUnsafeMutableBytes { buffer -> Bool in
        guard let context = CGContext(data: buffer.baseAddress, width: width, height: height, bitsPerComponent: 8,
                                      bytesPerRow: width * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                      bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { return false }
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        return true
    }
    if !drawn { fail("stats: cannot decode \(args[2])", 1) }
    var nonblack = 0
    var luma = 0.0
    for i in stride(from: 0, to: pixels.count, by: 4) {
        let r = pixels[i], g = pixels[i + 1], b = pixels[i + 2]
        if r > 16 || g > 16 || b > 16 { nonblack += 1 }
        luma += 0.2126 * Double(r) + 0.7152 * Double(g) + 0.0722 * Double(b)
    }
    let count = Double(width * height)
    print("{\"width\": \(width), \"height\": \(height), \"nonblack\": \(Double(nonblack) / count), \"mean_luma\": \(luma / count)}")
case "diff":
    // window_tool diff <a.png> <b.png>: the fraction of pixels whose largest channel difference
    // exceeds 24 (same-size images; a size change counts as fully changed).
    guard args.count == 4 else { fail("usage: window_tool diff <a> <b>", 64) }
    func load(_ path: String) -> (Int, Int, [UInt8]) {
        guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { fail("diff: cannot read \(path)", 1) }
        let w = image.width, h = image.height
        var px = [UInt8](repeating: 0, count: w * h * 4)
        px.withUnsafeMutableBytes { b in
            CGContext(data: b.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                      space: CGColorSpace(name: CGColorSpace.sRGB)!,
                      bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
                .draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
        }
        return (w, h, px)
    }
    let a = load(CommandLine.arguments[2]), b = load(CommandLine.arguments[3])
    if a.0 != b.0 || a.1 != b.1 { print("{\"changed\": 1.0}"); exit(0) }
    var changed = 0
    for i in stride(from: 0, to: a.2.count, by: 4) {
        let d = max(abs(Int(a.2[i]) - Int(b.2[i])), abs(Int(a.2[i + 1]) - Int(b.2[i + 1])), abs(Int(a.2[i + 2]) - Int(b.2[i + 2])))
        if d > 24 { changed += 1 }
    }
    print("{\"changed\": \(Double(changed) / Double(a.0 * a.1))}")
default:
    fail("usage: window_tool window <pid> | stats <png> | diff <a> <b>", 64)
}
