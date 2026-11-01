// NDS-AUDIO: implementacion de sonido para Nintendo DSi (sin SDL_audiolib).
// Efectos: WAV convertidos a 8 bits con signo, reproducidos en canales de hardware.
// Musica: WAV PCM leido por trozos y enviado a un anillo que reproduce el ARM7.
#include "engine/sound.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include <SDL.h>

#include "engine/assets.hpp"
#include "engine/sound_defs.hpp"
#include "options.h"

extern "C" {
int nds_snd_init(void);
int nds_snd_play(const void *pcm, int bytes, int is16, int freq, int vol, int pan);
void nds_snd_kill(int ch);
int nds_mus_start(int freq, int stereo, int (*rd)(void *buf, int bytes));
void nds_mus_stop(void);
void nds_mus_volume(int vol);
}

namespace devilution {

bool gbSndInited;
bool gbMusicOn = true;
bool gbSoundOn = true;
_music_id sgnMusicTrack = NUM_MUSIC;

namespace {

struct WavInfo {
	int channels = 0;
	int bits = 0;
	int rate = 0;
	uint32_t dataOff = 0;
	uint32_t dataLen = 0;
};

uint32_t Le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t Le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

bool ParseWav(const uint8_t *h, size_t n, size_t fileSize, WavInfo &w)
{
	if (n < 12 || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0)
		return false;
	size_t off = 12;
	bool haveFmt = false;
	while (off + 8 <= n) {
		const uint32_t sz = Le32(h + off + 4);
		if (std::memcmp(h + off, "fmt ", 4) == 0 && off + 8 + 16 <= n) {
			if (Le16(h + off + 8) != 1) // solo PCM
				return false;
			w.channels = Le16(h + off + 10);
			w.rate = static_cast<int>(Le32(h + off + 12));
			w.bits = Le16(h + off + 22);
			haveFmt = true;
		} else if (std::memcmp(h + off, "data", 4) == 0) {
			w.dataOff = static_cast<uint32_t>(off + 8);
			w.dataLen = std::min<uint32_t>(sz, static_cast<uint32_t>(fileSize - w.dataOff));
			return haveFmt && (w.bits == 8 || w.bits == 16) && (w.channels == 1 || w.channels == 2) && w.rate > 0;
		}
		off += 8 + sz + (sz & 1);
	}
	return false;
}

/** Volumen logaritmico (centesimas de dB) a lineal 0..127, como VolumeLogToLinear de soundsample.cpp. */
int LogToLin127(int logVol, int logMin)
{
	if (logVol <= logMin)
		return 0;
	if (logVol > 0)
		logVol = 0;
	const float scaled = (static_cast<float>(logVol - logMin) / static_cast<float>(-logMin)) * 10000.F - 10000.F;
	const float lin = std::pow(10.F, scaled / 3321.9281F);
	return std::clamp(static_cast<int>(lin * 127.F + 0.5F), 0, 127);
}

int PanTo127(int logPan)
{
	if (logPan == 0)
		return 64;
	const float factor = 1.F - std::pow(10.F, static_cast<float>(-std::abs(logPan)) / 6000.F);
	const float signedFactor = logPan < 0 ? -factor : factor;
	return std::clamp(64 + static_cast<int>(signedFactor * 63.F), 0, 127);
}

// ---- musica ----
std::optional<AssetHandle> MusFile;
WavInfo MusInfo;
uint32_t MusPos;
uint8_t MusTmp[2048];

int MusRead(void *buf, int bytes)
{
	if (!MusFile || MusInfo.dataLen == 0)
		return 0;
	int16_t *out = static_cast<int16_t *>(buf);
	int outSamples = bytes / 2;
	int done = 0;
	const int bytesPerSample = MusInfo.bits / 8;
	while (done < outSamples) {
		if (MusPos >= MusInfo.dataLen) { // bucle: volver al principio de los datos
			if (!MusFile->seek(static_cast<long>(MusInfo.dataOff)))
				break;
			MusPos = 0;
		}
		int want = (outSamples - done) * bytesPerSample;
		want = std::min<int>(want, static_cast<int>(sizeof(MusTmp)));
		want = std::min<int>(want, static_cast<int>(MusInfo.dataLen - MusPos));
		want -= want % (bytesPerSample * MusInfo.channels);
		if (want <= 0) {
			MusPos = MusInfo.dataLen;
			continue;
		}
		if (!MusFile->read(MusTmp, static_cast<size_t>(want)))
			break;
		MusPos += static_cast<uint32_t>(want);
		const int n = want / bytesPerSample;
		if (bytesPerSample == 2) {
			std::memcpy(out + done, MusTmp, static_cast<size_t>(n) * 2);
		} else {
			for (int i = 0; i < n; i++)
				out[done + i] = static_cast<int16_t>((MusTmp[i] - 128) << 8);
		}
		done += n;
	}
	return done * 2;
}

int MusicVolume127()
{
	return LogToLin127(*GetOptions().Audio.musicVolume, VOLUME_MIN);
}

const char *const SpawnMusicTracks[NUM_MUSIC] = {
	"music\\stowne.wav", "music\\slvla.wav", "music\\slvla.wav", "music\\slvla.wav",
	"music\\slvla.wav", "music\\dlvlf.wav", "music\\dlvle.wav", "music\\sintro.wav",
};
const char *const MusicTracks[NUM_MUSIC] = {
	"music\\dtowne.wav", "music\\dlvla.wav", "music\\dlvlb.wav", "music\\dlvlc.wav",
	"music\\dlvld.wav", "music\\dlvlf.wav", "music\\dlvle.wav", "music\\dintro.wav",
};

} // namespace

// ---- NdsSample ----
bool NdsSample::IsPlaying()
{
	return handle >= 0 && static_cast<int32_t>(SDL_GetTicks() - endTick) < 0;
}

bool NdsSample::PlayWithVolumeAndPan(int logSoundVolume, int logUserVolume, int logPan)
{
	if (pcm == nullptr)
		return false;
	const int vol = LogToLin127(logSoundVolume + logUserVolume * (ATTENUATION_MIN / VOLUME_MIN), ATTENUATION_MIN);
	if (vol <= 0)
		return false;
	handle = nds_snd_play(pcm, bytes, is16, freq, vol, PanTo127(logPan));
	endTick = SDL_GetTicks() + static_cast<uint32_t>(lengthMs) + 20;
	return handle >= 0;
}

void NdsSample::Stop()
{
	if (IsPlaying())
		nds_snd_kill(handle);
	handle = -1;
}

void NdsSample::Release()
{
	Stop();
	std::free(pcm);
	pcm = nullptr;
	bytes = 0;
}

// ---- API de sound.h ----
void ClearDuplicateSounds() { }

void snd_play_snd(TSnd *pSnd, int lVolume, int lPan, int userVolume)
{
	if (pSnd == nullptr || !gbSoundOn)
		return;
	const uint32_t tc = SDL_GetTicks();
	if (tc - pSnd->start_tc < 80)
		return;
	pSnd->DSB.PlayWithVolumeAndPan(lVolume, userVolume, lPan);
	pSnd->start_tc = tc;
}

std::expected<std::unique_ptr<TSnd>, std::string> SoundFileLoadWithStatus(const char *path, bool /*stream*/)
{
	auto snd = std::make_unique<TSnd>();
	snd->start_tc = SDL_GetTicks() - 80 - 1;
	if (!gbSndInited)
		return snd;
	size_t size = 0;
	AssetHandle handle = OpenAsset(path, size);
	if (!handle.ok() || size < 44)
		return snd; // sin sonido, pero sin error fatal
	uint8_t *file = static_cast<uint8_t *>(std::malloc(size));
	if (file == nullptr)
		return snd;
	WavInfo w;
	if (!handle.read(file, size) || !ParseWav(file, size, size, w)) {
		std::free(file);
		return snd;
	}
	const int bps = w.bits / 8;
	const uint32_t frames = w.dataLen / static_cast<uint32_t>(bps * w.channels);
	const uint32_t ndsStep = w.rate >= 22050 ? 2U : 1U; /* NDS-DIETA1: 22 kHz -> 11 kHz */
	const uint32_t outFrames = frames / ndsStep;
	const uint32_t outBytes = (outFrames + 3) & ~3U;
	int8_t *out = static_cast<int8_t *>(std::malloc(outBytes));
	if (out == nullptr) {
		std::free(file);
		return snd;
	}
	const uint8_t *src = file + w.dataOff;
	for (uint32_t i = 0; i < outFrames; i++) {
		int acc = 0;
		for (uint32_t k = 0; k < ndsStep; k++) {
			for (int c = 0; c < w.channels; c++) {
				const uint8_t *s = src + ((i * ndsStep + k) * w.channels + c) * bps;
				acc += bps == 2 ? static_cast<int16_t>(Le16(s)) >> 8 : static_cast<int>(s[0]) - 128;
			}
		}
		out[i] = static_cast<int8_t>(acc / (w.channels * static_cast<int>(ndsStep)));
	}
	for (uint32_t i = outFrames; i < outBytes; i++)
		out[i] = 0;
	std::free(file);
	snd->DSB.pcm = out;
	snd->DSB.bytes = static_cast<int>(outBytes);
	snd->DSB.is16 = 0;
	snd->DSB.freq = static_cast<int>(w.rate / ndsStep);
	snd->DSB.lengthMs = static_cast<int>(static_cast<uint64_t>(frames) * 1000 / static_cast<uint32_t>(w.rate));
	return snd;
}

std::unique_ptr<TSnd> sound_file_load(const char *path, bool stream)
{
	return std::move(SoundFileLoadWithStatus(path, stream)).value();
}

TSnd::~TSnd()
{
	DSB.Release();
}

void snd_init()
{
	GetOptions().Audio.soundVolume.SetValue(std::clamp(*GetOptions().Audio.soundVolume, VOLUME_MIN, VOLUME_MAX));
	gbSoundOn = *GetOptions().Audio.soundVolume > VOLUME_MIN;
	GetOptions().Audio.musicVolume.SetValue(std::clamp(*GetOptions().Audio.musicVolume, VOLUME_MIN, VOLUME_MAX));
	gbMusicOn = *GetOptions().Audio.musicVolume > VOLUME_MIN;
	gbSndInited = nds_snd_init() != 0;
}

void snd_deinit()
{
	music_stop();
	gbSndInited = false;
}

void music_stop()
{
	nds_mus_stop();
	MusFile = std::nullopt;
	sgnMusicTrack = NUM_MUSIC;
}

#ifdef __NDS__
extern "C" void nds_extra_mark(const char *name); /* NDS-MUSICAS2 */
#endif
void music_start(_music_id nTrack)
{
#ifdef __NDS__ /* NDS-MUSICAS: apuntar la musica como escuchada (Extras > Soundtrack) */
	if (nTrack == TMUSIC_INTRO) { /* NDS-MUSDESBLOQUEO: las de los pisos se apuntan al entrar en el nivel */
		char ndsN[8] = "mus0";
		ndsN[3] = static_cast<char>('0' + static_cast<int>(nTrack));
		nds_extra_mark(ndsN);
	}
#endif
	music_stop();
	if (!gbSndInited || !gbMusicOn || nTrack >= NUM_MUSIC)
		return;
	const char *trackPath = HaveFullMusic() ? MusicTracks[nTrack] : SpawnMusicTracks[nTrack];
	size_t size = 0;
	AssetHandle handle = OpenAsset(trackPath, size);
	if (!handle.ok())
		return;
	uint8_t header[1024];
	const size_t headerLen = std::min<size_t>(sizeof(header), size);
	if (!handle.read(header, headerLen) || !ParseWav(header, headerLen, size, MusInfo))
		return;
	if (!handle.seek(static_cast<long>(MusInfo.dataOff)))
		return;
	MusFile.emplace(std::move(handle));
	MusPos = 0;
	nds_mus_volume(MusicVolume127());
	if (!nds_mus_start(MusInfo.rate, MusInfo.channels == 2 ? 1 : 0, MusRead)) {
		music_stop();
		return;
	}
	sgnMusicTrack = nTrack;
}

void sound_disable_music(bool disable)
{
	if (disable) {
		const _music_id track = sgnMusicTrack;
		music_stop();
		sgnMusicTrack = track;
	} else if (sgnMusicTrack != NUM_MUSIC) {
		music_start(sgnMusicTrack);
	}
}

int sound_get_or_set_music_volume(int volume)
{
	if (volume == 1)
		return *GetOptions().Audio.musicVolume;
	GetOptions().Audio.musicVolume.SetValue(volume);
	nds_mus_volume(MusicVolume127());
	return *GetOptions().Audio.musicVolume;
}

int sound_get_or_set_sound_volume(int volume)
{
	if (volume == 1)
		return *GetOptions().Audio.soundVolume;
	GetOptions().Audio.soundVolume.SetValue(volume);
	return *GetOptions().Audio.soundVolume;
}

int SoundGetOrSetAudioCuesVolume(int volume)
{
	if (volume == 1)
		return *GetOptions().Audio.audioCuesVolume;
	GetOptions().Audio.audioCuesVolume.SetValue(volume);
	return *GetOptions().Audio.audioCuesVolume;
}

void music_mute() { nds_mus_volume(0); }
void music_unmute() { nds_mus_volume(MusicVolume127()); }

_music_id GetLevelMusic(dungeon_type dungeonType)
{
	switch (dungeonType) {
	case DTYPE_TOWN: return TMUSIC_TOWN;
	case DTYPE_CATHEDRAL: return TMUSIC_CATHEDRAL;
	case DTYPE_CATACOMBS: return TMUSIC_CATACOMBS;
	case DTYPE_CAVES: return TMUSIC_CAVES;
	case DTYPE_HELL: return TMUSIC_HELL;
	case DTYPE_NEST: return TMUSIC_NEST;
	case DTYPE_CRYPT: return TMUSIC_CRYPT;
	default: return TMUSIC_INTRO;
	}
}

} // namespace devilution
