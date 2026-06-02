import os
from falcor import *
from tqdm import tqdm

SCENE = os.environ.get("FALCOR_SCENE", "")
OUTPUT_DIR = os.environ.get("FALCOR_OUTPUT_DIR", "")
START_FRAME = int(os.environ.get("FALCOR_START_FRAME", "32"))
NUM_FRAMES = int(os.environ.get("FALCOR_NUM_FRAMES", "60"))
FRAMERATE = int(os.environ.get("FALCOR_FRAMERATE", "30"))
RESOLUTION = [int(v) for v in os.environ.get("FALCOR_RESOLUTION", "1280x720").split("x")]


def render_graph_collect():
    g = RenderGraph("CollectRTXDI")
    g.addPass(createPass("GBufferRT", {'useAlphaTest': True}), "GBufferRT")
    g.addPass(createPass("RTXDIPass"), "RTXDIPass")
    g.addEdge("GBufferRT.vbuffer", "RTXDIPass.vbuffer")
    g.addEdge("GBufferRT.mvec", "RTXDIPass.mvec")
    g.addEdge("GBufferRT.mvecW", "RTXDIPass.mvecW")
    g.markOutput("RTXDIPass.inputRadiance")
    g.markOutput("RTXDIPass.brdf")
    g.markOutput("RTXDIPass.reservoir")
    g.markOutput("GBufferRT.guideNormalW")
    g.markOutput("GBufferRT.emissive")
    g.markOutput("GBufferRT.linearZ")
    g.markOutput("GBufferRT.mvec")
    g.markOutput("GBufferRT.disocclusion")
    return g


g = render_graph_collect()
m.addGraph(g)
m.loadScene(SCENE)

m.resizeFrameBuffer(*RESOLUTION)
m.ui = False
m.clock.framerate = FRAMERATE
m.clock.time = 0
m.clock.pause()

m.frameCapture.baseFilename = "rtxdi"
if OUTPUT_DIR:
    m.frameCapture.outputDir = OUTPUT_DIR

for frame in range(0, START_FRAME):
    m.clock.frame = frame
    m.renderFrame()

for frame in tqdm(range(START_FRAME, START_FRAME + NUM_FRAMES), desc="RTXDI", unit="frame", dynamic_ncols=True):
    m.clock.frame = frame
    m.renderFrame()
    m.frameCapture.capture()

exit()
