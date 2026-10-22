"""Analyzes a Ray Regen Bridge frame capture (menu: Diagnostics > Capture frame).

usage: python analyze_capture.py <capture folder> [--tile 32]

Reads every .dds the capture wrote (RGBA16F) and capture.json, and writes to <folder>/analysis:
  report.md        numbers per stage, per distance / roughness / albedo band and the worst screen regions
  *.png            previews of each texture and heatmaps (brightness gain, color shift, clamping, residual noise)

The denoiser only removes noise, so averaged over a region its output should match its input. Every comparison is
done on tile means (noise averages out) at each stage:
  game color -> prepass signals (split must rebuild the color) -> denoiser in vs out -> composed vs game color
"""
import argparse
import json
import math
import os
import sys

import numpy as np
from PIL import Image

LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
MIN_ALBEDO = 0.02
FLAG_PARTICLE_LAYER = 0x8
FLAG_TRANSPARENCY_LAYER = 0x40
FLAG_SSS_GUIDE = 0x100
SPLIT_ALBEDO = 0


def read_dds(path):
    with open(path, 'rb') as f:
        data = f.read()
    height, width = int.from_bytes(data[12:16], 'little'), int.from_bytes(data[16:20], 'little')
    pixels = np.frombuffer(data, dtype=np.float16, offset=148, count=width * height * 4)
    return pixels.reshape(height, width, 4).astype(np.float32)


def luma(rgb):
    return rgb[..., :3] @ LUMA


def tile_mean(img, tile, mask=None):
    """Mean per tile (H/tile, W/tile[, C]); with a mask, mean over masked texels and the count per tile."""
    h, w = img.shape[:2]
    th, tw = h // tile, w // tile
    img = img[:th * tile, :tw * tile]
    shape = (th, tile, tw, tile) + img.shape[2:]
    if mask is None:
        return img.reshape(shape).mean(axis=(1, 3)), None
    m = mask[:th * tile, :tw * tile].astype(np.float32)
    mw = m.reshape(th, tile, tw, tile)
    count = mw.sum(axis=(1, 3))
    weighted = (img * (m[..., None] if img.ndim == 3 else m)).reshape(shape).sum(axis=(1, 3))
    denom = np.maximum(count, 1.0)
    return weighted / (denom[..., None] if img.ndim == 3 else denom), count


def tonemap(rgb, key=None):
    rgb = np.nan_to_num(np.maximum(rgb[..., :3], 0.0), nan=0.0, posinf=0.0)
    lum = luma(rgb)
    if key is None:
        key = max(float(np.exp(np.mean(np.log(lum + 1e-4)))), 1e-4)
    x = rgb * (0.18 / key)
    x = x / (1.0 + x)
    return np.clip(x ** (1 / 2.2) * 255, 0, 255).astype(np.uint8), key


def save_png(path, img8):
    Image.fromarray(img8).save(path)


def diverging(values, limit):
    """Blue (negative) - white (0) - red (positive), clipped at +-limit, uint8 RGB."""
    t = np.clip(values / limit, -1, 1)
    r = np.where(t > 0, 1.0, 1.0 + t)
    g = 1.0 - np.abs(t)
    b = np.where(t < 0, 1.0, 1.0 - t)
    return (np.stack([r, g, b], -1) * 255).astype(np.uint8)


def sequential(values, limit):
    t = np.clip(values / limit, 0, 1)
    return (np.stack([t, t * 0.6, 1 - t], -1) * 255).astype(np.uint8)


def upscale(tiles, tile):
    return np.kron(tiles, np.ones((tile, tile, 1), dtype=tiles.dtype)) if tiles.ndim == 3 else \
        np.kron(tiles, np.ones((tile, tile), dtype=tiles.dtype))


def overlay(base8, heat8, alpha=0.55):
    h, w = heat8.shape[:2]
    return (base8[:h, :w].astype(np.float32) * (1 - alpha) + heat8.astype(np.float32) * alpha).astype(np.uint8)


def pct(x):
    return f'{100.0 * x:+.1f}%'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('folder')
    parser.add_argument('--tile', type=int, default=32)
    args = parser.parse_args()

    folder = args.folder
    meta = json.load(open(os.path.join(folder, 'capture.json'), encoding='utf-8'))
    out = os.path.join(folder, 'analysis')
    os.makedirs(out, exist_ok=True)

    rw, rh = meta['render']
    tex = {}
    for t in meta['textures']:
        path = os.path.join(folder, t['name'] + '.dds')
        if os.path.exists(path):
            tex[t['name']] = read_dds(path)[:rh, :rw] if t['width'] >= rw and t['height'] >= rh else read_dds(path)

    report = [f'# Capture {os.path.basename(os.path.normpath(folder))}', '',
              f"render {rw}x{rh}, display {meta['display'][0]}x{meta['display'][1]}, flags 0x{meta['flags']:x}, "
              f"pre-exposure {meta['preExposure']}, split mode {meta['splitMode']}, max radiance {meta['maxRadiance']}, "
              f"demodulation {meta['demodulation']}, mirror {meta['mirrorRoughness']}, "
              f"demod fade {meta['demodFadeDistance']}, far passthrough {meta['farPassthrough']}", '',
              'Textures: ' + ', '.join(sorted(tex)), '']

    # Previews: the game's color sets the exposure every color-like texture is shown with
    color = tex['color'][..., :3]
    composed = tex['composed'][..., :3]
    color8, key = tonemap(color)
    save_png(os.path.join(out, 'color.png'), color8)
    save_png(os.path.join(out, 'composed.png'), tonemap(composed, key)[0])
    for name in ('diffuse_albedo', 'specular_albedo'):
        if name in tex:
            save_png(os.path.join(out, name + '.png'), np.clip(tex[name][..., :3] ** (1 / 2.2) * 255, 0, 255).astype(np.uint8))
    for name in ('diffuse_in', 'diffuse_out', 'specular_in', 'specular_out', 'particles', 'before_transparency',
                 'emissive'):
        if name in tex:
            save_png(os.path.join(out, name + '.png'), tonemap(tex[name][..., :3])[0])

    pre = float(meta['preExposure']) or 1.0
    da = tex['diffuse_albedo'][..., :3]
    sa = tex['specular_albedo'][..., :3]
    packed = tex['packed_normals']
    roughness = packed[..., 2]
    material = packed[..., 3]
    depth = tex['linear_depth'][..., 0]
    sky_depth = float(meta['skyDepth'])
    din, dout = tex['diffuse_in'], tex['diffuse_out']
    sin_, sout = tex['specular_in'], tex['specular_out']
    active = (din[..., 3] >= 0) | (sin_[..., 3] >= 0)
    sky = depth >= sky_depth * 0.999
    skin = np.abs(material - 1 / 3) < 0.05
    sentinel = material > 0.5

    report += ['## Coverage', '',
               f'- active for the denoiser: {active.mean():.1%}, sky {sky.mean():.1%}, '
               f'skin {skin.mean():.1%}, sentinel {sentinel.mean():.1%}', '']

    # 1. Split check: modulations as the prepass computes them (fsrd_common.hlsli GetModulation)
    demod = float(meta['demodulation'])
    dmod = np.maximum(da, MIN_ALBEDO) ** demod
    smod = (np.maximum(sa, MIN_ALBEDO) if meta['splitMode'] == SPLIT_ALBEDO else np.maximum(da + sa, MIN_ALBEDO)) ** demod
    emissive = np.maximum(tex['emissive'][..., :3], 0) if 'emissive' in tex else 0.0
    color_lin = np.maximum(color / pre - emissive, 0)
    rebuilt_in = np.where(din[..., 3:4] >= 0, din[..., :3] * dmod, 0) + np.where(sin_[..., 3:4] >= 0, sin_[..., :3] * smod, 0)
    split_err = np.abs(luma(rebuilt_in) - luma(color_lin)) / np.maximum(luma(color_lin), 1e-3)
    report += ['## 1. Prepass split (signals x modulation must rebuild the color)', '',
               f'- median relative error on active texels: {np.median(split_err[active]):.3%}, '
               f'99th percentile {np.percentile(split_err[active], 99):.1%}', '']

    # 2. Clamping: the denoiser caps signals at max radiance
    max_rad = float(meta['maxRadiance'])
    sig_max = np.maximum(np.where(din[..., 3:4] >= 0, din[..., :3], 0).max(-1), np.where(sin_[..., 3:4] >= 0, sin_[..., :3], 0).max(-1))
    clamped = active & (sig_max > max_rad)
    lost = np.where(clamped, np.maximum(sig_max - max_rad, 0) / np.maximum(sig_max, 1e-6), 0)
    report += ['## 2. Signals above max radiance (clamped by the denoiser)', '',
               f'- texels over {max_rad:g}: {clamped.mean():.2%} of the frame; '
               f'median energy lost on them {np.median(lost[clamped]) if clamped.any() else 0:.1%}', '']
    clamp_tiles, _ = tile_mean(clamped.astype(np.float32), args.tile)
    save_png(os.path.join(out, 'heat_clamped.png'), overlay(color8, upscale(sequential(clamp_tiles, 0.25), args.tile)))

    # 3. Denoiser bias: tile mean out vs in, per signal, on texels active in that signal
    report += ['## 3. Denoiser bias (tile mean out / in, active texels only)', '']
    for name, sig_in, sig_out in (('diffuse', din, dout), ('specular', sin_, sout)):
        mask = sig_in[..., 3] >= 0
        if not mask.any():
            report.append(f'- {name}: no active texels')
            continue
        m_in, count = tile_mean(np.minimum(sig_in[..., :3], max_rad), args.tile, mask)
        m_out, _ = tile_mean(sig_out[..., :3], args.tile, mask)
        valid = count > args.tile * args.tile * 0.25
        gain = np.log2(np.maximum(luma(m_out), 1e-5) / np.maximum(luma(m_in), 1e-5))
        g = gain[valid]
        report.append(f'- {name}: median gain {pct(2 ** np.median(g) - 1)}, tiles above +20%: '
                      f'{np.mean(g > math.log2(1.2)):.1%}, below -20%: {np.mean(g < math.log2(0.8)):.1%}')
        save_png(os.path.join(out, f'heat_denoiser_{name}.png'),
                 overlay(color8, upscale(diverging(np.where(valid, gain, 0), 1.0), args.tile)))
    report.append('')

    # 4. End to end: composed vs game color (both pre-exposed), tile means over the whole frame
    lum_c, _ = tile_mean(luma(color), args.tile)
    lum_o, _ = tile_mean(luma(composed), args.tile)
    gain = np.log2(np.maximum(lum_o, 1e-5) / np.maximum(lum_c, 1e-5))
    rgb_c, _ = tile_mean(color, args.tile)
    rgb_o, _ = tile_mean(composed, args.tile)
    chroma_c = rgb_c / np.maximum(rgb_c.sum(-1, keepdims=True), 1e-5)
    chroma_o = rgb_o / np.maximum(rgb_o.sum(-1, keepdims=True), 1e-5)
    shift = np.abs(chroma_o - chroma_c).sum(-1)
    lit = lum_c > np.percentile(lum_c, 5)
    report += ['## 4. Composed vs game color (whole frame, tile means)', '',
               f'- frame mean luminance: game {luma(color).mean():.4f}, composed {luma(composed).mean():.4f} '
               f'({pct(luma(composed).mean() / max(luma(color).mean(), 1e-6) - 1)})',
               f'- tiles brighter than +15%: {np.mean(gain[lit] > math.log2(1.15)):.1%}, darker than -15%: '
               f'{np.mean(gain[lit] < math.log2(0.85)):.1%}',
               f'- color shift (sum of chromaticity differences): median {np.median(shift[lit]):.3f}, '
               f'tiles above 0.05: {np.mean(shift[lit] > 0.05):.1%}', '']
    save_png(os.path.join(out, 'heat_gain.png'), overlay(color8, upscale(diverging(gain, 1.0), args.tile)))
    save_png(os.path.join(out, 'heat_color_shift.png'), overlay(color8, upscale(sequential(shift, 0.15), args.tile)))

    th, tw = gain.shape
    order = np.argsort(np.abs(gain).ravel())[::-1][:12]
    report += ['Worst tiles (x, y in render pixels, gain, color shift, mean depth, roughness, albedo luma):', '']
    d_t, _ = tile_mean(np.where(sky, np.nan, depth), args.tile)
    r_t, _ = tile_mean(roughness, args.tile)
    a_t, _ = tile_mean(luma(da + sa), args.tile)
    for idx in order:
        y, x = divmod(int(idx), tw)
        report.append(f'- ({x * args.tile}, {y * args.tile}): {pct(2 ** gain[y, x] - 1)}, shift {shift[y, x]:.3f}, '
                      f'depth {d_t[y, x]:.1f} m, roughness {r_t[y, x]:.2f}, albedo {a_t[y, x]:.3f}')
    report.append('')

    # 5. Bands: where does the gain come from (distance, roughness, albedo)
    def band_table(title, values, edges, unit):
        rows = [f'### {title}', '', '| band | share of frame | game luma | composed luma | gain |', '|---|---|---|---|---|']
        lc, lo = luma(color), luma(composed)
        for lo_e, hi_e in zip(edges[:-1], edges[1:]):
            m = active & (values >= lo_e) & (values < hi_e)
            if m.mean() < 0.002:
                continue
            a, b = lc[m].mean(), lo[m].mean()
            rows.append(f'| {lo_e:g}-{hi_e:g}{unit} | {m.mean():.1%} | {a:.4f} | {b:.4f} | {pct(b / max(a, 1e-6) - 1)} |')
        return rows + ['']

    report += ['## 5. Gain by band (active texels)', '']
    report += band_table('Distance', depth, [0, 2, 5, 10, 25, 50, 100, 250, 1e9], ' m')
    report += band_table('Roughness', roughness, [0, 0.1, 0.2, 0.35, 0.5, 0.75, 1.01], '')
    report += band_table('Albedo luma (diffuse + specular)', luma(da + sa), [0, 0.02, 0.05, 0.1, 0.2, 0.4, 0.7, 2], '')
    report += band_table('Material (0 surface, 0.33 skin)', material, [-0.1, 0.1, 0.5, 1.1], '')

    # 6. Residual noise: high frequency left in the composed image vs the game's, per tile
    def hf(img):
        l = luma(img)
        pad = np.pad(l, 1, mode='edge')
        blur = sum(pad[dy:dy + l.shape[0], dx:dx + l.shape[1]] for dy in range(3) for dx in range(3)) / 9.0
        return np.abs(l - blur) / np.maximum(blur, 1e-3)

    hf_c, _ = tile_mean(hf(color), args.tile)
    hf_o, _ = tile_mean(hf(composed), args.tile)
    ratio = hf_o / np.maximum(hf_c, 1e-4)
    report += ['## 6. Residual noise (relative 3x3 high frequency, composed / game)', '',
               f'- median {np.median(ratio):.2f} (1 = as noisy as the input, ~0 = smooth); '
               f'tiles keeping more than half the input noise: {np.mean(ratio > 0.5):.1%}']
    if skin.any():
        sk_c, _ = tile_mean(hf(color), args.tile, skin)
        sk_o, cnt = tile_mean(hf(composed), args.tile, skin)
        v = cnt > 16
        if v.any():
            report.append(f'- skin tiles: median {np.median(sk_o[v] / np.maximum(sk_c[v], 1e-4)):.2f}')
    report.append('')
    save_png(os.path.join(out, 'heat_noise.png'), overlay(color8, upscale(sequential(ratio, 1.0), args.tile)))

    report += ['## Images', '',
               '- color.png / composed.png: game input and our result, same exposure',
               '- heat_gain.png: composed / game brightness per tile (red brighter, blue darker, full color = 2x)',
               '- heat_color_shift.png: chromaticity change per tile (orange = strong)',
               '- heat_denoiser_diffuse/specular.png: denoiser output / input per tile',
               '- heat_clamped.png: share of texels above max radiance',
               '- heat_noise.png: noise left after denoising (orange = as noisy as the input)', '']

    with open(os.path.join(out, 'report.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(report))
    print('\n'.join(report))


if __name__ == '__main__':
    sys.exit(main())
