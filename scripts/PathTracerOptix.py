import os
from falcor import *
from tqdm import tqdm

SCENE = os.environ.get("FALCOR_SCENE", "Arcade/Arcade.pyscene")
OUTPUT_DIR = os.environ.get("FALCOR_OUTPUT_DIR", "")
START_FRAME = int(os.environ.get("FALCOR_START_FRAME", "32"))
NUM_FRAMES = int(os.environ.get("FALCOR_NUM_FRAMES", "60"))
SPP = int(os.environ.get("FALCOR_SPP", "256"))
FRAMERATE = int(os.environ.get("FALCOR_FRAMERATE", "30"))
RESOLUTION = [int(v) for v in os.environ.get("FALCOR_RESOLUTION", "1280x720").split("x")]


def render_graph_gt():
    g = RenderGraph("CollectGT")
    g.addPass(createPass("VBufferRT", {'samplePattern': 'Center', 'useAlphaTest': True}), "VBufferRT")
    g.addPass(createPass("PathTracer", {'samplesPerPixel': 1}), "PathTracer")
    g.addPass(createPass("AccumulatePass", {'enabled': True, 'precisionMode': 'Single'}), "AccumulatePass")
    g.addPass(createPass("OptixDenoiser", {'model': 'HDR', 'denoiseAlpha': False}), "OptixDenoiser")
    g.addEdge("VBufferRT.vbuffer", "PathTracer.vbuffer")
    g.addEdge("VBufferRT.viewW", "PathTracer.viewW")
    g.addEdge("VBufferRT.mvec", "PathTracer.mvec")
    g.addEdge("PathTracer.color", "AccumulatePass.input")
    g.addEdge("AccumulatePass.output", "OptixDenoiser.color")
    g.addEdge("PathTracer.albedo", "OptixDenoiser.albedo")
    g.addEdge("PathTracer.guideNormal", "OptixDenoiser.normal")
    g.markOutput("OptixDenoiser.output")
    return g


g = render_graph_gt()
m.addGraph(g)
m.loadScene(SCENE)

m.resizeFrameBuffer(*RESOLUTION)
m.ui = False
m.clock.framerate = FRAMERATE
m.clock.time = 0
m.clock.pause()

m.frameCapture.baseFilename = "gt"
if OUTPUT_DIR:
    m.frameCapture.outputDir = OUTPUT_DIR

accumulate = g.getPass("AccumulatePass")

progress = tqdm(total=NUM_FRAMES * SPP, desc="PathTraced", unit="spp", dynamic_ncols=True)
for frame in range(START_FRAME, START_FRAME + NUM_FRAMES):
    m.clock.frame = frame
    progress.set_postfix(frame=frame)
    accumulate.reset()
    for _ in range(SPP):
        m.renderFrame()
        progress.update(1)
    m.frameCapture.capture()
progress.close()

exit()
