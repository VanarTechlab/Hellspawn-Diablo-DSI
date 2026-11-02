# NDS-DSV (+DSV2: sonido adelantado): convierte las cinematicas .smk de Diablo a un formato propio para la DSi (.dsv)
#  - imagen 256x192, 256 colores (una paleta por video), solo se guarda lo que cambia
#  - sonido mono 22050 Hz 8 bits
# Uso: python3 convertir_cinematicas.py [carpeta con los .smk] [carpeta de salida]
#   por defecto lee /mnt/f/devilutionx/diabdat/gendata y escribe en /mnt/f/devilutionx/dsv
# Necesita ffmpeg (sudo apt install ffmpeg)
import os, sys, struct, subprocess, tempfile

SRC = sys.argv[1] if len(sys.argv) > 1 else '/mnt/f/devilutionx/diabdat/gendata'
DST = sys.argv[2] if len(sys.argv) > 2 else '/mnt/f/devilutionx/dsv'
W, H = 256, 192
FRAME = W * H
UNITS = FRAME // 8  # bloques de 8 pixeles
RATE = 22050
LEAD = 10  # fotogramas de sonido adelantado


def probe_fps(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                          'stream=r_frame_rate', '-of', 'csv=p=0', path], capture_output=True, text=True).stdout.strip()
    num, _, den = out.partition('/')
    fps = float(num) / float(den or 1)
    if not (1 <= fps <= 60):
        raise RuntimeError('velocidad de video rara: ' + out)
    return fps


def audio_of(path):
    r = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-vn', '-ac', '1', '-ar', str(RATE), '-f', 'u8',
                        '-acodec', 'pcm_u8', '-'], capture_output=True)
    return r.stdout if r.returncode == 0 else b''


def encode_delta(prev, cur):
    """pares (u16 saltar, u16 copiar) en bloques de 8 pixeles + datos copiados"""
    pv = memoryview(prev).cast('Q')
    cv = memoryview(cur).cast('Q')
    out = bytearray()
    pos = 0
    while pos < UNITS:
        s = pos
        while pos < UNITS and pv[pos] == cv[pos]:
            pos += 1
        skip = pos - s
        c = pos
        # copiar hasta encontrar 2 bloques iguales seguidos (asi no se corta por bloques sueltos)
        while pos < UNITS and not (pv[pos] == cv[pos] and (pos + 1 >= UNITS or pv[pos + 1] == cv[pos + 1])):
            pos += 1
        copy = pos - c
        out += struct.pack('<HH', skip, copy)
        out += cur[c * 8:(c + copy) * 8]
    return out


def convert(src, dst):
    fps = probe_fps(src)
    with tempfile.TemporaryDirectory() as td:
        pal_png = os.path.join(td, 'pal.png')
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', src, '-vf',
                        'scale=%d:%d:flags=lanczos,palettegen=max_colors=256:stats_mode=full' % (W, H), pal_png], check=True)
        proc = subprocess.Popen(['ffmpeg', '-v', 'error', '-i', src, '-i', pal_png, '-lavfi',
                                 '[0:v]scale=%d:%d:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=4' % (W, H),
                                 '-f', 'rawvideo', '-pix_fmt', 'pal8', '-'], stdout=subprocess.PIPE)
        audio = audio_of(src)
        frames = []
        palette = None
        prev = bytes(FRAME)
        while True:
            blk = proc.stdout.read(FRAME + 1024)
            if len(blk) < FRAME + 1024:
                break
            cur = blk[:FRAME]
            if palette is None:
                palette = blk[FRAME:]
            frames.append(encode_delta(prev, cur))
            prev = cur
        proc.wait()
    if not frames:
        raise RuntimeError('no salio ningun fotograma')
    pal15 = bytearray()
    for i in range(256):
        b, g, r = palette[i * 4], palette[i * 4 + 1], palette[i * 4 + 2]
        pal15 += struct.pack('<H', (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10))
    frame_us = int(round(1000000 / fps))
    with open(dst, 'wb') as f:
        f.write(b'DSV1' + struct.pack('<HHIIIHHII', W, H, len(frames), frame_us, RATE if audio else 0, 8, 1, 0, 0))
        f.write(pal15)
        apos = 0
        for i, vid in enumerate(frames):
            # NDS-DSV2: el sonido va adelantado LEAD fotogramas (~0,7 s de colchon), asi no se corta si
            # la imagen se retrasa un poco (pasaba al final de la intro, con mucho movimiento)
            aend = min(len(audio), int(round((i + 1 + LEAD) * RATE / fps)))
            if i == len(frames) - 1:
                aend = len(audio)
            ab = audio[apos:aend]
            while len(ab) > 60000:  # (no deberia pasar)
                ab = ab[:60000]
            apos = aend
            rec = struct.pack('<H', len(ab)) + ab + vid
            f.write(struct.pack('<I', len(rec)) + rec)
    return len(frames), fps, os.path.getsize(dst)


def main():
    if not os.path.isdir(SRC):
        print('No encuentro', SRC, '(monta la SD: sudo mount -t drvfs F: /mnt/f)'); sys.exit(1)
    os.makedirs(DST, exist_ok=True)
    vids = sorted(x for x in os.listdir(SRC) if x.lower().endswith('.smk'))
    if not vids:
        print('No hay .smk en', SRC); sys.exit(1)
    total = 0
    for v in vids:
        out = os.path.join(DST, os.path.splitext(v)[0].lower() + '.dsv')
        print('convirtiendo', v, '...', flush=True)
        n, fps, size = convert(os.path.join(SRC, v), out)
        total += size
        print('   %d fotogramas a %.1f fps (%.0f s), %.1f MB' % (n, fps, n / fps, size / 1048576))
    print('listo: %d videos, %.1f MB en total, en %s' % (len(vids), total / 1048576, DST))


if __name__ == '__main__':
    main()
