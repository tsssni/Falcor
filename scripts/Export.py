import bpy
import json
import os
import shutil
import sys
import tempfile

import numpy as np
import OpenImageIO as oiio


def parse_args():
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    fbx = out = texdir = None
    glass = True
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--textures-dir':
            i += 1
            texdir = argv[i]
        elif a == '--no-glass':
            glass = False
        elif fbx is None:
            fbx = a
        elif out is None:
            out = a
        i += 1
    if not fbx or not out:
        print('usage: blender -b --factory-startup --python Export.py -- '
              '<input.fbx> <output.gltf> [--textures-dir DIR] [--no-glass]')
        sys.exit(1)
    return os.path.abspath(fbx), os.path.abspath(out), texdir, glass


def import_fbx(fbx):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    try:
        bpy.ops.wm.fbx_import(filepath=fbx)
    except AttributeError:
        bpy.ops.import_scene.fbx(filepath=fbx, use_image_search=True)


def remap_missing_images(fbx, texdir):
    candidates = [d for d in [texdir,
                              os.path.join(os.path.dirname(fbx), 'Textures'),
                              os.path.dirname(fbx)] if d and os.path.isdir(d)]
    missing = []
    for img in bpy.data.images:
        if img.source != 'FILE':
            continue
        if os.path.exists(bpy.path.abspath(img.filepath)):
            continue
        base = os.path.basename(img.filepath.replace('\\', '/'))
        stem = os.path.splitext(base)[0]
        for cand in [os.path.join(d, n) for d in candidates
                     for n in (base, stem + '.png')]:
            if os.path.exists(cand):
                img.filepath = cand
                break
        else:
            missing.append(base)
    return missing


def convert_dds_images(tmpdir):
    converted = 0
    failed = []
    for img in bpy.data.images:
        if img.source != 'FILE':
            continue
        path = bpy.path.abspath(img.filepath)
        if not path.lower().endswith('.dds'):
            continue
        base = os.path.basename(path)
        stem = os.path.splitext(base)[0]
        sibling = os.path.join(os.path.dirname(path), stem + '.png')
        if not os.path.exists(sibling):
            dst = sibling if os.access(os.path.dirname(path), os.W_OK) \
                else os.path.join(tmpdir, stem + '.png')
            buf = oiio.ImageBuf(path)
            if buf.has_error:
                failed.append(base)
                continue
            if buf.nchannels == 2:
                buf = oiio.ImageBufAlgo.channels(buf, (0, 1, 0.0), ('R', 'G', 'B'))
            if not buf.write(dst):
                failed.append(base)
                continue
            sibling = dst
        img.filepath = sibling
        img.reload()
        converted += 1
    return converted, failed


def image_pixels(img):
    px = np.empty(img.size[0] * img.size[1] * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    return px.reshape(-1, 4)


def flip_normal_maps(flip_dir):
    normal_images = set()
    for mat in bpy.data.materials:
        if not mat.use_nodes:
            continue
        for n in mat.node_tree.nodes:
            if n.type != 'NORMAL_MAP' or not n.inputs['Color'].is_linked:
                continue
            src = n.inputs['Color'].links[0].from_node
            if src.type == 'TEX_IMAGE' and src.image:
                normal_images.add(src.image)
    for img in normal_images:
        px = image_pixels(img)
        px[:, 1] = 1.0 - px[:, 1]
        if px[:, 2].max() < 0.01:
            x = px[:, 0] * 2.0 - 1.0
            y = px[:, 1] * 2.0 - 1.0
            z = np.sqrt(np.clip(1.0 - x * x - y * y, 0.0, 1.0))
            px[:, 2] = z * 0.5 + 0.5
        img.pixels.foreach_set(px.ravel())
        img.filepath_raw = os.path.join(flip_dir, os.path.basename(
            img.filepath.replace('\\', '/')) or img.name + '.png')
        img.file_format = 'PNG'
        img.save()
    return len(normal_images)


def rewire_orm():
    count = 0
    for mat in bpy.data.materials:
        if not mat.use_nodes:
            continue
        nt = mat.node_tree
        bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        if not bsdf:
            continue
        for sn in [n for n in nt.nodes if n.type == 'TEX_IMAGE' and n.image
                   and '_Specular' in os.path.basename(n.image.filepath.replace('\\', '/'))]:
            sn.image.colorspace_settings.name = 'Non-Color'
            for l in list(nt.links):
                if l.from_node == sn:
                    nt.links.remove(l)
            sep = nt.nodes.new('ShaderNodeSeparateColor')
            sep.location = (bsdf.location.x - 300, bsdf.location.y - 320)
            nt.links.new(sn.outputs['Color'], sep.inputs['Color'])
            nt.links.new(sep.outputs['Green'], bsdf.inputs['Roughness'])
            nt.links.new(sep.outputs['Blue'], bsdf.inputs['Metallic'])
            count += 1
    return count


def apply_glass():
    names = []
    for mat in bpy.data.materials:
        if not mat.use_nodes or 'glass' not in mat.name.lower():
            continue
        nt = mat.node_tree
        bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        if not bsdf:
            continue
        bsdf.inputs['Transmission Weight'].default_value = 1.0
        bsdf.inputs['IOR'].default_value = 1.5
        if not bsdf.inputs['Roughness'].is_linked:
            bsdf.inputs['Roughness'].default_value = 0.0
        for l in list(bsdf.inputs['Alpha'].links):
            nt.links.remove(l)
        bsdf.inputs['Alpha'].default_value = 1.0
        names.append(mat.name)
    return names


def classify_alpha():
    classes = {}
    cache = {}
    for mat in bpy.data.materials:
        if not mat.use_nodes:
            continue
        nt = mat.node_tree
        bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        if not bsdf or not bsdf.inputs['Base Color'].is_linked:
            continue
        src = bsdf.inputs['Base Color'].links[0].from_node
        if src.type != 'TEX_IMAGE' or not src.image:
            continue
        img = src.image
        if img.name not in cache:
            a = image_pixels(img)[::4, 3]
            if a.min() >= 0.98:
                cls = 'OPAQUE'
            elif ((a > 0.04) & (a < 0.96)).mean() < 0.05:
                cls = 'MASK'
            else:
                cls = 'BLEND'
            cache[img.name] = cls
        classes[mat.name] = cache[img.name]
    return classes


def export_gltf(out):
    os.makedirs(os.path.dirname(out), exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=out,
        export_format='GLTF_SEPARATE',
        export_texture_dir='textures',
        export_image_format='AUTO',
        export_apply=True,
    )


def patch_gltf(out, alpha_classes):
    doc = json.load(open(out))
    occ = 0
    for m in doc.get('materials', []):
        pbr = m.get('pbrMetallicRoughness', {})
        mrt = pbr.get('metallicRoughnessTexture')
        if mrt is not None and 'occlusionTexture' not in m:
            m['occlusionTexture'] = {'index': mrt['index']}
            if 'texCoord' in mrt:
                m['occlusionTexture']['texCoord'] = mrt['texCoord']
            occ += 1
        if 'KHR_materials_transmission' in m.get('extensions', {}):
            m.pop('alphaMode', None)
            m.pop('alphaCutoff', None)
            bcf = pbr.get('baseColorFactor')
            if bcf and bcf[3] < 1.0:
                bcf[3] = 1.0
            continue
        cls = alpha_classes.get(m.get('name'))
        if cls == 'OPAQUE':
            m.pop('alphaMode', None)
            m.pop('alphaCutoff', None)
        elif cls == 'MASK':
            m['alphaMode'] = 'MASK'
            m['alphaCutoff'] = 0.5
        elif cls == 'BLEND':
            m['alphaMode'] = 'BLEND'
    json.dump(doc, open(out, 'w'), separators=(',', ':'))
    return occ, len(doc.get('materials', []))


def main():
    fbx, out, texdir, glass = parse_args()
    import_fbx(fbx)
    missing = remap_missing_images(fbx, texdir)
    if missing:
        print('WARNING missing textures:', missing)
    tmpdir = tempfile.mkdtemp(prefix='falcor_export_')
    try:
        dds, dds_failed = convert_dds_images(tmpdir)
        if dds_failed:
            print('WARNING dds conversion failed:', dds_failed)
        orm = rewire_orm()
        flipped = flip_normal_maps(tmpdir)
        glass_names = apply_glass() if glass else []
        alpha_classes = classify_alpha()
        export_gltf(out)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    occ, nmat = patch_gltf(out, alpha_classes)
    print('materials:', nmat)
    print('dds converted:', dds)
    print('orm rewired:', orm)
    print('normals flipped:', flipped)
    print('occlusion linked:', occ)
    print('glass transmission:', glass_names)
    from collections import Counter
    print('alpha:', dict(Counter(alpha_classes.values())))
    print('written:', out)


main()
