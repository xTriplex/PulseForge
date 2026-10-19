import Foundation
import Metal

guard let device = MTLCreateSystemDefaultDevice() else {
    print("PULSEFORGE_METAL_PROBE=unavailable reason=no-default-metal-device")
    exit(0)
}

guard let commandQueue = device.makeCommandQueue() else {
    print("PULSEFORGE_METAL_PROBE=unavailable reason=no-metal-command-queue device=\(device.name)")
    exit(0)
}

let source = """
    #include <metal_stdlib>
    using namespace metal;

    kernel void PulseForgeCapabilityProbe(device uint* Value [[buffer(0)]], uint Index [[thread_position_in_grid]])
    {
        Value[Index] = 0x50464D54;
    }
    """

do {
    let library = try device.makeLibrary(source: source, options: nil)
    guard let function = library.makeFunction(name: "PulseForgeCapabilityProbe") else {
        fputs("PULSEFORGE_METAL_PROBE=error reason=kernel-function-missing\n", stderr)
        exit(1)
    }

    let pipeline = try device.makeComputePipelineState(function: function)
    guard let buffer = device.makeBuffer(length: MemoryLayout<UInt32>.stride, options: .storageModeShared) else {
        print("PULSEFORGE_METAL_PROBE=unavailable reason=shared-buffer-allocation-failed device=\(device.name)")
        exit(0)
    }
    guard let commandBuffer = commandQueue.makeCommandBuffer(),
          let encoder = commandBuffer.makeComputeCommandEncoder() else {
        print("PULSEFORGE_METAL_PROBE=unavailable reason=command-encoding-unavailable device=\(device.name)")
        exit(0)
    }

    encoder.setComputePipelineState(pipeline)
    encoder.setBuffer(buffer, offset: 0, index: 0)
    encoder.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1),
                                 threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
    encoder.endEncoding()
    commandBuffer.commit()
    commandBuffer.waitUntilCompleted()

    guard commandBuffer.status == .completed else {
        fputs("PULSEFORGE_METAL_PROBE=error reason=command-buffer-\(commandBuffer.status) device=\(device.name)\n", stderr)
        exit(1)
    }

    let result = buffer.contents().assumingMemoryBound(to: UInt32.self).pointee
    guard result == 0x50464D54 else {
        fputs("PULSEFORGE_METAL_PROBE=error reason=unexpected-result-\(result) device=\(device.name)\n", stderr)
        exit(1)
    }

    print("PULSEFORGE_METAL_PROBE=compute-passed device=\(device.name) registryID=\(device.registryID)")
} catch {
    fputs("PULSEFORGE_METAL_PROBE=error reason=\(error) device=\(device.name)\n", stderr)
    exit(1)
}
