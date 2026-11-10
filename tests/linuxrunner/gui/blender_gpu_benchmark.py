"""Run inside a real Blender GUI, never with --background.

Reports the actual renderer, tests offscreen pixels, saves a repeatable scene,
and times synchronous viewport redraws. Redraw throughput is not display FPS.
Example: blender --factory-startup --python-exit-code 45 --python this_file.py
Set LR_BLENDER_RESULTS to a new, writable JSON path in a disposable fixture.
"""
import json
import ctypes
import math
import os
from pathlib import Path
import time
import traceback

import bpy
import gpu
from mathutils import Quaternion

started = time.monotonic()
output = Path(os.environ["LR_BLENDER_RESULTS"])
iterations = int(os.environ.get("LR_BLENDER_ITERATIONS", "24"))
if not 1 <= iterations <= 120:
    raise ValueError("Benchmark iterations must be between 1 and 120")
shading = os.environ.get("LR_BLENDER_SHADING", "SOLID")
if shading not in ("SOLID", "WIREFRAME"):
    raise ValueError("Use an explicitly labelled SOLID or WIREFRAME workload")
if output.exists() or output.is_symlink():
    raise RuntimeError("Refusing to overwrite benchmark evidence")
result = {
    "blender_version": bpy.app.version_string,
    "background": bpy.app.background,
    "scope": "ARM64 Linux Blender GUI inside LinuxRunner; not an iPhone benchmark",
    "status": "FAIL",
    "detailed_phase_profiling": os.environ.get("LR_BLENDER_PROFILE") == "1",
}


def publish():
    temporary = output.with_suffix(".pending")
    with temporary.open("x") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.rename(output)
    print("BLENDER_BENCHMARK_RESULT", json.dumps(result), flush=True)


def benchmark():
    ready_file = os.environ.get("LR_BLENDER_UI_READY")
    if ready_file and not Path(ready_file).exists():
        return 0.25
    try:
        if bpy.app.background:
            raise RuntimeError("A real GUI GPU context is required")
        result.update(
            renderer=gpu.platform.renderer_get(),
            vendor=gpu.platform.vendor_get(),
            graphics_version=gpu.platform.version_get(),
            backend=gpu.platform.backend_type_get(),
            seconds_to_python=time.monotonic() - started,
        )
        renderer = result["renderer"].lower()
        print("BLENDER_BENCHMARK_RENDERER", json.dumps(result), flush=True)
        result["software_renderer"] = any(
            value in renderer for value in ("llvmpipe", "softpipe", "software rasterizer")
        )
        # The existing Mesa/Metal bridge intentionally retains Mesa's renderer
        # string. Its independent draw/fallback counters are required before
        # attributing any of this workload to Metal.
        try:
            if not result["detailed_phase_profiling"]:
                raise AttributeError("Detailed profiling requires its own labelled run")
            bridge_report = ctypes.CDLL(None).lrMesaMetalReport
            bridge_report.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
            def report_bridge():
                buffer = ctypes.create_string_buffer(8192)
                bridge_report(buffer, len(buffer))
                return buffer.value.decode(errors="replace")
            result["bridge_before"] = report_bridge()
        except AttributeError:
            report_bridge = None
        def bridge_statistics():
            try:
                function = ctypes.CDLL(None).lrMesaMetalStatistics
                function.argtypes = [ctypes.POINTER(ctypes.c_uint64)] * 6
                values = [ctypes.c_uint64() for _ in range(6)]
                if function(*(ctypes.byref(value) for value in values)):
                    return dict(zip(("metal_draws", "verified", "mismatched", "mesa_draws", "submissions", "gpu_nanoseconds"),
                                    (value.value for value in values)))
            except AttributeError:
                pass
            return None
        result["bridge_counters_before"] = bridge_statistics()
        # A readable texture from this context is separate evidence from a
        # renderer string; no Metal claim follows merely from successful EGL.
        offscreen = gpu.types.GPUOffScreen(32, 32)
        try:
            with offscreen.bind():
                framebuffer = gpu.state.active_framebuffer_get()
                framebuffer.clear(color=(0.25, 0.5, 0.75, 1.0))
                pixel = framebuffer.read_color(8, 8, 1, 1, 4, 0, "UBYTE")
                actual = list(pixel.to_list()[0][0])
                result["offscreen_pixel"] = actual
                result["offscreen_pixel_pass"] = all(
                    abs(a - b) <= 1 for a, b in zip(actual, (64, 128, 191, 255))
                ) and len(actual) == 4
        finally:
            offscreen.free()

        bpy.ops.object.select_all(action="SELECT")
        bpy.ops.object.delete(use_global=False)
        for y in range(4):
            for x in range(4):
                bpy.ops.mesh.primitive_cube_add(size=1, location=((x - 1.5) * 1.4, (y - 1.5) * 1.4, 0))
        result["scene_mesh_objects"] = len(bpy.data.objects)
        window = bpy.context.window
        area = next(a for a in window.screen.areas if a.type == "VIEW_3D")
        region = next(r for r in area.regions if r.type == "WINDOW")
        area.spaces.active.shading.type = shading
        result["viewport_shading"] = shading
        result["window_size"] = [window.width, window.height]
        area.spaces.active.region_3d.view_distance = 12
        bpy.ops.wm.save_as_mainfile(filepath=str(output.with_suffix(".blend")))
        samples = []
        with bpy.context.temp_override(window=window, area=area, region=region):
            for index in range(iterations):
                before = time.monotonic()
                area.spaces.active.region_3d.view_rotation = Quaternion((0, 0, 1), index * math.pi / 48)
                area.tag_redraw()
                bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=1)
                samples.append(time.monotonic() - before)
                if sum(samples) > 90:
                    break
        result.update(
            redraw_samples_seconds=samples,
            redraw_count=len(samples),
            requested_redraw_count=iterations,
            redraw_seconds=sum(samples),
            synchronous_redraws_per_second=len(samples) / sum(samples),
            metric_note="Synchronous whole-window redraw throughput; includes interpreter, CPU, GPU and swap work. Not monitor FPS or a Cycles benchmark.",
            status="PASS" if result["offscreen_pixel_pass"] and len(samples) == iterations else "FAIL",
        )
        if report_bridge:
            result["bridge_workload"] = report_bridge()
        result["bridge_counters_after"] = bridge_statistics()
    except Exception:
        result["error"] = traceback.format_exc()
    finally:
        result["total_python_seconds"] = time.monotonic() - started
        publish()
    # Keep the actual application visible for the native framebuffer probe.
    return None


bpy.app.timers.register(benchmark, first_interval=2.0)
