// NDS-MP2 + NDS-MP3 + NDS-MP4 + NDS-MP5 + NDS-MP6 + NDS-MP7 + NDS-MP8 + NDS-MP9 + NDS-MP10 + NDS-MP11 + NDS-MP13 + NDS-MP14 + NDS-MP15 + NDS-MP16 + NDS-MP17 + NDS-MP18 + NDS-MP19: conector "DSi local" sobre DSWiFi (multijugador local DS a DS).
//
// Transporte (NDS-MP4): se usa el mecanismo CMD/REPLY del multijugador de DS, el mismo del ejemplo
// local_multiplayer que funciono entre DSi y 3DS:
//  - el anfitrion manda una trama CMD cada pocos ms (con datos para el cliente o vacia)
//  - cada CMD hace que el cliente conteste automaticamente con su trama REPLY preparada
// Por ahora: 1 anfitrion + 1 cliente (2 jugadores).
// Encima va una capa fiable y ordenada: numero de secuencia, acuse acumulado, reenvio y troceo.
// Los paquetes "fuera de banda" (pedir/anunciar partidas) van sin garantias.
#include "dvlnet/protocol_nds.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

#include <dswifi9.h>
#include <nds.h>

extern "C" unsigned nds_usec(void);
extern "C" int nds_wifi_ok;
extern "C" void (*nds_net_hook)(void);
// NDS-MP13: la libreria reparte los paquetes en Wifi_Update(), que normalmente llama su propio hilo
// (wifi_update_thread). En el juego ese hilo no llega a ejecutarse, asi que la llamamos nosotros.
extern "C" void Wifi_Update(void);

namespace devilution::net {

namespace {

constexpr u32 GameId = 0x44564C58; // "DVLX"
constexpr int MaxClients = 1;
constexpr size_t PktSize = 256;      // tamano maximo que se intenta (NDS-MP5: se prueba hacia abajo)
constexpr size_t HeaderSize = 10; // NDS-MP15: +2 bytes (eco de la sesion del otro lado)
size_t Pkt = 0;                      // tamano aceptado por la libreria (0 = aun no)
size_t FragPayloadNow()
{
	return (Pkt > HeaderSize ? Pkt : 32) - HeaderSize;
}
// NDS-MP5: la libreria devuelve -1 si el tamano es demasiado grande: se prueba de mayor a menor
// NDS-MP7: con 224 el cliente no recibia ningun CMD; se prueba con tramas pequenas (como el ejemplo)
// NDS-MP16: lo de 224 era en realidad la falta de Wifi_Update (NDS-MP13); se vuelve a tramas grandes
constexpr size_t SizeList[] = { 192, 128, 96, 64, 48, 32, 24, 16 };
constexpr size_t MaxInFlight = 32; // NDS-MP16: fragmentos sin acuse como mucho
constexpr uint32_t ResendMs = 150;
constexpr uint32_t CmdEveryUs = 16000; // NDS-MP6: un CMD cada 16 ms, y solo con un cliente conectado
constexpr int RxSlots = 32;
constexpr int NumIds = 17; // 0..16 (1 = anfitrion, 2.. = cliente con AID id-1)

enum : uint8_t {
	KindIdle = 'N',
	KindData = 'D',
	KindAck = 'A',
	KindOob = 'O',
};

// NDS-MP15: nds_usec() da la vuelta cada 65,5 s (cuenta de ms de 16 bits). Eso disparaba en falso el
// vigilante de la radio justo al dar la vuelta. Aqui se extiende a 32 bits.
uint32_t ClkRawLast = 0, ClkUsHi = 0, ClkMsHi = 0;
void ClockUpdate(uint32_t &us, uint32_t &ms)
{
	const uint32_t raw = nds_usec(); // 0 .. 65535999
	if (raw < ClkRawLast && ClkRawLast - raw > 32768000u) {
		ClkUsHi += 65536000u;
		ClkMsHi += 65536u;
	}
	ClkRawLast = raw;
	us = ClkUsHi + raw;
	ms = ClkMsHi + raw / 1000u;
}
uint32_t NowMs()
{
	uint32_t us, ms;
	ClockUpdate(us, ms);
	return ms;
}
uint32_t NowUs()
{
	uint32_t us, ms;
	ClockUpdate(us, ms);
	return us;
}

// ---- registro en sd:/devilutionx/red.txt ----
int LogLines = 0;
// NDS-MP16: las lineas se guardan en memoria y se escriben en la SD solo desde el hilo del juego
// (el hilo de la red no debe tocar la SD por si el juego esta leyendo un archivo en ese momento)
std::string LogBuf;
bool LogStarted = false;
void NetLog(const char *fmt, ...)
{
	if (LogLines >= 1500 || LogBuf.size() > 16384)
		return;
	char line[200];
	int n = std::snprintf(line, sizeof(line), "%7u ", static_cast<unsigned>(NowMs()));
	va_list ap;
	va_start(ap, fmt);
	if (n > 0 && n < static_cast<int>(sizeof(line)))
		std::vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);
	LogBuf += line;
	LogBuf += '\n';
	LogLines++;
}
void FlushLog()
{
	if (LogBuf.empty())
		return;
	FILE *f = std::fopen("sd:/devilutionx/red.txt", LogStarted ? "a" : "w");
	if (f == nullptr)
		return;
	LogStarted = true;
	std::fwrite(LogBuf.data(), 1, LogBuf.size(), f);
	std::fclose(f);
	LogBuf.clear();
}
uint32_t StatTx = 0, StatRx = 0, StatOobTx = 0, StatOobRx = 0, StatCmdRx = 0;

// ---- anillo de recepcion (lo llena la interrupcion) ----
struct RxSlot {
	uint16_t len;
	uint8_t from;
	alignas(4) uint8_t data[PktSize + 4];
};
RxSlot RxRing[RxSlots];
volatile uint32_t RxWrite = 0;
volatile uint32_t RxRead = 0;
volatile uint32_t RxDropped = 0;
volatile uint32_t CmdArrived = 0; // el cliente cuenta los CMD (para preparar la siguiente respuesta)

void RxPush(uint8_t from, int base, int len)
{
	if (len <= 0 || static_cast<size_t>(len) > PktSize) { // (el anillo admite hasta PktSize)
		RxDropped = RxDropped + 1;
		return;
	}
	const uint32_t w = RxWrite;
	if (w - RxRead >= RxSlots) {
		RxDropped = RxDropped + 1;
		return;
	}
	RxSlot &s = RxRing[w % RxSlots];
	Wifi_RxRawReadPacket(static_cast<u32>(base), static_cast<u32>((len + 1) & ~1), s.data);
	s.len = static_cast<uint16_t>(len);
	s.from = from;
	RxWrite = w + 1;
}

volatile uint32_t HandlerCalls = 0; // NDS-MP9: llamadas al receptor (de cualquier tipo)
volatile int LastType = -9, LastLen = -1;
void FromClientHandler(Wifi_MPPacketType type, int aid, int base, int len)
{
	HandlerCalls = HandlerCalls + 1;
	LastType = static_cast<int>(type);
	LastLen = len;
	if (type == WIFI_MPTYPE_REPLY && aid >= 1 && aid <= 15)
		RxPush(static_cast<uint8_t>(aid + 1), base, len);
}

void FromHostHandler(Wifi_MPPacketType type, int base, int len)
{
	HandlerCalls = HandlerCalls + 1;
	LastType = static_cast<int>(type);
	LastLen = len;
	if (type == WIFI_MPTYPE_CMD) {
		CmdArrived = CmdArrived + 1;
		RxPush(1, base, len);
	}
}

// ---- estado por companero ----
struct Frag {
	uint16_t seq;
	bool sent;
	uint32_t sentAt;
	buffer_t bytes;
};

struct Peer {
	bool active = false;
	uint16_t txNext = 0;
	std::deque<Frag> txq; // fragmentos sin acuse, en orden
	std::deque<buffer_t> oobq;
	uint16_t rxExpected = 0;
	bool needAck = false;
	buffer_t pending; // NDS-MP16: flujo de salida aun sin trocear: [largo 2 bytes][mensaje]...
	buffer_t stream;  // NDS-MP16: flujo de entrada en orden, aun sin separar en mensajes
	uint16_t remoteTok = 0; // NDS-MP15: sesion del otro lado (cambia si reinicia la conexion)
};

enum class Role : uint8_t { None, Host, Client };
enum class ClientState : uint8_t { Init, WaitMode, Scan, Connecting, Connected };
enum class HostState : uint8_t { Init, WaitMode, Ready };

Role CurRole = Role::None;
ClientState CState = ClientState::Init;
HostState HState = HostState::Init;
uint16_t LastMask = 0;
uint32_t LastCmdUs = 0;
uint32_t LastCmdSeen = 0;
uint32_t ConnectStartMs = 0; // NDS-MP19
std::array<Peer, NumIds> Peers;
std::deque<std::pair<uint8_t, buffer_t>> Delivered; // mensajes completos (fiables)
std::deque<std::pair<uint8_t, buffer_t>> OobQueue;  // fuera de banda recibidos
std::deque<uint8_t> DisconnectQueue;
bool InService = false;
bool InNetThread = false; // NDS-MP16
uint16_t MyTok = 0;                  // NDS-MP15: nuestra sesion (nueva en cada SetRole)
std::array<bool, NumIds> AwaitDrop {}; // NDS-MP15: esperando a que el juego procese la desconexion
uint32_t LogMsgIn = 0, LogMsgOut = 0, LogReq = 0; // NDS-MP15: limitar lineas repetidas del registro

bool SeqLess(uint16_t a, uint16_t b)
{
	return static_cast<int16_t>(a - b) < 0;
}

void ResetPeer(uint8_t id)
{
	Peers[id] = Peer {};
}

void ActivatePeer(uint8_t id)
{
	ResetPeer(id);
	Peers[id].active = true;
}

// NDS-MP14: si se cae el enlace wifi, el estado del companero se conserva unos segundos por si
// vuelve (asi la partida no se corta por un tiron de la radio)
constexpr uint32_t GraceMs = 15000;
std::array<uint32_t, NumIds> LostAt {};
void DropPeer(uint8_t id);
void SoftLose(uint8_t id)
{
	if (Peers[id].active && LostAt[id] == 0) {
		LostAt[id] = NowMs() | 1;
		NetLog("enlace con %u perdido: se espera %u s a que vuelva", id, static_cast<unsigned>(GraceMs / 1000));
	}
}
void CheckGrace()
{
	const uint32_t now = NowMs();
	for (uint8_t id = 1; id < NumIds; id++) {
		if (LostAt[id] != 0 && now - LostAt[id] > GraceMs) {
			NetLog("companero %u no volvio: desconectado", id);
			LostAt[id] = 0;
			DropPeer(id);
		}
	}
}

void DropPeer(uint8_t id)
{
	if (!Peers[id].active)
		return;
	ResetPeer(id);
	DisconnectQueue.push_back(id);
}

/** Elige lo siguiente que hay que mandar a "id" y lo escribe en f (cabecera + datos). Devuelve el tamano. */
size_t BuildFrame(uint8_t id, std::array<uint8_t, PktSize> &f)
{
	Peer &p = Peers[id];
	const uint32_t now = NowMs();
	uint8_t kind = KindIdle;
	uint8_t flags = 0;
	uint16_t seq = 0;
	const buffer_t *payload = nullptr;
	buffer_t oob;
	if (p.active && !p.oobq.empty()) {
		oob = std::move(p.oobq.front());
		p.oobq.pop_front();
		kind = KindOob;
		payload = &oob;
		StatOobTx++;
	} else if (p.active) {
		for (Frag &fr : p.txq) {
			if (!fr.sent || now - fr.sentAt >= ResendMs) {
				fr.sent = true;
				fr.sentAt = now;
				kind = KindData;
				seq = fr.seq;
				payload = &fr.bytes;
				break;
			}
		}
		// NDS-MP16: trozo nuevo del flujo: varios mensajes pequenos viajan juntos en una trama
		if (kind == KindIdle && !p.pending.empty() && p.txq.size() < MaxInFlight) {
			const size_t n = std::min(FragPayloadNow(), p.pending.size());
			Frag fr;
			fr.seq = p.txNext++;
			fr.sent = true;
			fr.sentAt = now;
			fr.bytes.assign(p.pending.begin(), p.pending.begin() + n);
			p.pending.erase(p.pending.begin(), p.pending.begin() + n);
			p.txq.push_back(std::move(fr));
			kind = KindData;
			seq = p.txq.back().seq;
			payload = &p.txq.back().bytes;
		}
		if (kind == KindIdle && p.needAck)
			kind = KindAck;
	}
	f[0] = kind;
	f[1] = flags;
	f[2] = static_cast<uint8_t>(seq & 0xFF);
	f[3] = static_cast<uint8_t>(seq >> 8);
	f[4] = static_cast<uint8_t>(p.rxExpected & 0xFF);
	f[5] = static_cast<uint8_t>(p.rxExpected >> 8);
	f[6] = static_cast<uint8_t>(MyTok & 0xFF); // NDS-MP15: sesion
	f[7] = static_cast<uint8_t>(MyTok >> 8);
	f[8] = static_cast<uint8_t>(p.remoteTok & 0xFF); // NDS-MP15: la sesion del otro lado que conocemos
	f[9] = static_cast<uint8_t>(p.remoteTok >> 8);
	size_t len = HeaderSize;
	if (payload != nullptr) {
		const size_t n = std::min(payload->size(), FragPayloadNow());
		std::memcpy(f.data() + HeaderSize, payload->data(), n);
		len += n;
	}
	if (kind != KindIdle)
		p.needAck = false; // el acuse va dentro de toda trama
	if (kind != KindIdle)
		StatTx++;
	return len;
}

void HandleFrame(uint8_t from, const uint8_t *d, size_t len)
{
	if (len < HeaderSize || from == 0 || from >= NumIds)
		return;
	if (AwaitDrop[from])
		return; // NDS-MP15: el otro lado reinicio; se ignora hasta que el juego procese la desconexion
	const uint16_t tok = static_cast<uint16_t>(d[6] | (d[7] << 8));
	Peer &p = Peers[from];
	if (p.active && p.remoteTok != 0 && tok != p.remoteTok) {
		// NDS-MP15: el companero empezo una conexion nueva (salio y volvio a entrar): no se puede
		// seguir con los numeros de secuencia viejos, se empieza de cero
		NetLog("companero %u reinicio la conexion: se empieza de cero", from);
		LostAt[from] = 0;
		DropPeer(from);
		AwaitDrop[from] = true;
		return;
	}
	if (!p.active) {
		if (CurRole == Role::Host)
			ActivatePeer(from);
		else
			return;
	}
	if (p.remoteTok == 0)
		p.remoteTok = tok;
	// NDS-MP15: si el otro lado aun no conoce nuestra sesion actual, sus numeros (acuse/secuencia) son de
	// una conexion anterior: no se usan hasta que se ponga al dia (una ida y vuelta)
	const uint16_t echo = static_cast<uint16_t>(d[8] | (d[9] << 8));
	if (echo != MyTok)
		return;
	const uint8_t kind = d[0];
	if (kind == KindIdle)
		return;
	StatRx++;
	const uint16_t seq = static_cast<uint16_t>(d[2] | (d[3] << 8));
	const uint16_t ack = static_cast<uint16_t>(d[4] | (d[5] << 8));
	// acuse acumulado: todo lo anterior a "ack" ha llegado
	while (!p.txq.empty() && SeqLess(p.txq.front().seq, ack))
		p.txq.pop_front();
	if (kind == KindOob) {
		StatOobRx++;
		NetLog("OOB recibido de %u (%u bytes)", from, static_cast<unsigned>(len - HeaderSize));
		OobQueue.emplace_back(from, buffer_t(d + HeaderSize, d + len));
		return;
	}
	if (kind != KindData)
		return;
	if (seq == p.rxExpected) {
		// NDS-MP16: se anade al flujo y se sacan los mensajes completos
		p.stream.insert(p.stream.end(), d + HeaderSize, d + len);
		size_t off = 0;
		while (p.stream.size() - off >= 2) {
			const size_t L = static_cast<size_t>(p.stream[off] | (p.stream[off + 1] << 8));
			if (p.stream.size() - off - 2 < L)
				break;
			if (++LogMsgIn <= 20 || LogMsgIn % 100 == 0)
				NetLog("(n%u) mensaje completo de %u (%u bytes)", static_cast<unsigned>(LogMsgIn), from, static_cast<unsigned>(L));
			Delivered.emplace_back(from, buffer_t(p.stream.begin() + off + 2, p.stream.begin() + off + 2 + L));
			off += 2 + L;
		}
		if (off != 0)
			p.stream.erase(p.stream.begin(), p.stream.begin() + off);
		p.rxExpected++;
	}
	p.needAck = true;
}

void PollHostMask()
{
	const uint16_t mask = Wifi_MultiplayerGetClientMask();
	if (mask == LastMask)
		return;
	for (int aid = 1; aid <= 15; aid++) {
		const bool now = (mask & (1 << aid)) != 0;
		const bool before = (LastMask & (1 << aid)) != 0;
		const uint8_t id = static_cast<uint8_t>(aid + 1);
		if (now && !before) {
			if (Peers[id].active) {
				if (LostAt[id] != 0)
					NetLog("enlace con %u recuperado", id);
				LostAt[id] = 0; // NDS-MP14: vuelve el mismo companero, se sigue donde se estaba
			} else {
				ActivatePeer(id);
			}
		}
		if (!now && before)
			SoftLose(id); // NDS-MP14
	}
	NetLog("anfitrion: mascara de clientes 0x%04X", mask);
	LastMask = mask;
}

uint8_t HostClientId()
{
	for (uint8_t id = 2; id < NumIds; id++)
		if (Peers[id].active)
			return id;
	return 2;
}

uint32_t StatCmdTx = 0;
uint32_t WatchTx = 0, WatchAt = 0, TxAtLastCmd = 0;
/** NDS-MP14: si el ARM7 deja de transmitir (se vio en la DSi tras ~1 minuto), se reinicia el modo anfitrion */
bool HostWatchdog()
{
	const uint32_t now = NowMs();
	const uint32_t tx = Wifi_GetStats(WSTAT_TXPACKETS);
	// NDS-MP18: si el juego estuvo un rato sin atender la red (cargando, saliendo), es normal que no se
	// haya transmitido nada: se vuelve a empezar a contar en vez de dar la radio por trabada
	static uint32_t lastCall = 0;
	const bool gap = lastCall != 0 && now - lastCall > 200;
	lastCall = now;
	if (tx != WatchTx || WatchAt == 0 || gap) {
		WatchTx = tx;
		WatchAt = now;
		return false;
	}
	if (now - WatchAt < 1000)
		return false;
	NetLog("anfitrion: la radio dejo de transmitir (tx %u), reiniciando el modo anfitrion", static_cast<unsigned>(tx));
	for (uint8_t id = 2; id < NumIds; id++)
		SoftLose(id);
	Wifi_IdleMode();
	HState = HostState::Init;
	LastMask = 0;
	WatchAt = 0;
	return true;
}

void HostSendCmd()
{
	if ((LastMask & 0xFFFE) == 0)
		return; // NDS-MP6: sin clientes no se emite (no tapar los anuncios de la partida)
	if (HostWatchdog())
		return;
	const uint32_t nowUs = NowUs(); // NDS-MP15
	if (nowUs - LastCmdUs < CmdEveryUs)
		return;
	// NDS-MP14: no encolar otro CMD si el anterior aun no ha salido (como mucho se espera 60 ms)
	if (Wifi_GetStats(WSTAT_TXPACKETS) == TxAtLastCmd && nowUs - LastCmdUs < 60000)
		return;
	LastCmdUs = nowUs;
	TxAtLastCmd = Wifi_GetStats(WSTAT_TXPACKETS);
	std::array<uint8_t, PktSize> f;
	const size_t len = BuildFrame(HostClientId(), f);
	if (Wifi_MultiplayerHostCmdTxFrame(f.data(), len) >= 0)
		StatCmdTx++;
}

void ClientPrepareReply()
{
	const uint32_t c = CmdArrived;
	if (c == LastCmdSeen)
		return; // la respuesta preparada aun no se ha usado
	LastCmdSeen = c;
	std::array<uint8_t, PktSize> f;
	const size_t len = BuildFrame(1, f);
	Wifi_MultiplayerClientReplyTxFrame(f.data(), len);
}

void ClientStep()
{
	switch (CState) {
	case ClientState::Init:
		NetLog("cliente: modo cliente");
		Pkt = 0;
		for (size_t sz : SizeList) {
			if (Wifi_MultiplayerClientMode(sz) == 0) {
				Pkt = sz;
				break;
			}
		}
		NetLog("cliente: tamano de trama aceptado %u", static_cast<unsigned>(Pkt));
		Wifi_MultiplayerFromHostSetPacketHandler(FromHostHandler);
		CState = ClientState::WaitMode;
		break;
	case ClientState::WaitMode:
		if (Wifi_LibraryModeReady()) {
			Wifi_MultiplayerFromHostSetPacketHandler(FromHostHandler); // NDS-MP8: tras el cambio de modo
			NetLog("cliente: listo, buscando");
			Wifi_ScanMode();
			CState = ClientState::Scan;
		}
		break;
	case ClientState::Scan: {
		const int n = Wifi_GetNumAP();
		static uint32_t lastScanLog = 0;
		const bool logNow = NowMs() - lastScanLog >= 2000;
		if (logNow) {
			lastScanLog = NowMs();
			NetLog("cliente: %d redes vistas", n);
		}
		for (int i = 0; i < n; i++) {
			Wifi_AccessPoint ap;
			std::memset(&ap, 0, sizeof(ap));
			Wifi_GetAPData(i, &ap);
			if (logNow && i < 6)
				NetLog("  red %d: juego %08X admite %u jugadores %u/%u canal %d", i, static_cast<unsigned>(ap.nintendo.game_id),
				    ap.nintendo.allows_connections, ap.nintendo.players_current, ap.nintendo.players_max, ap.channel);
			if (ap.nintendo.game_id == GameId && ap.nintendo.allows_connections) {
				NetLog("cliente: partida encontrada, conectando (canal %d)", ap.channel);
				Wifi_ConnectOpenAP(&ap);
				ConnectStartMs = NowMs(); // NDS-MP19
				CState = ClientState::Connecting;
				break;
			}
		}
		break;
	}
	case ClientState::Connecting: {
		const int st = Wifi_AssocStatus();
		if (st == ASSOCSTATUS_ASSOCIATED) {
			Wifi_MultiplayerFromHostSetPacketHandler(FromHostHandler); // NDS-MP8: como el ejemplo, al conectar
			NetLog("cliente: CONECTADO al anfitrion");
			if (Peers[1].active && LostAt[1] != 0) {
				NetLog("enlace con el anfitrion recuperado");
				LostAt[1] = 0; // NDS-MP14
			} else {
				ActivatePeer(1);
			}
			LastCmdSeen = CmdArrived - 1; // preparar ya una respuesta
			CState = ClientState::Connected;
		} else if (st == ASSOCSTATUS_CANNOTCONNECT || NowMs() - ConnectStartMs > 5000) {
			// NDS-MP19: se vio a la 3DS quedarse para siempre "conectando" (estado %d sin cambiar).
			// Tras 5 s sin conectar se reinicia el modo cliente entero y se vuelve a buscar.
			NetLog("cliente: no se pudo conectar (estado %d), se reinicia el modo cliente", st);
			Wifi_IdleMode();
			CState = ClientState::Init;
		}
		break;
	}
	case ClientState::Connected:
		if (Wifi_AssocStatus() != ASSOCSTATUS_ASSOCIATED) {
			NetLog("cliente: conexion perdida");
			SoftLose(1); // NDS-MP14
			Wifi_ScanMode();
			CState = ClientState::Scan;
		}
		break;
	}
}

void HostStep()
{
	switch (HState) {
	case HostState::Init:
		NetLog("anfitrion: modo anfitrion");
		Pkt = 0;
		for (size_t sz : SizeList) {
			if (Wifi_MultiplayerHostMode(MaxClients, sz, sz) == 0) {
				Pkt = sz;
				break;
			}
		}
		NetLog("anfitrion: tamano de trama aceptado %u", static_cast<unsigned>(Pkt));
		Wifi_MultiplayerFromClientSetPacketHandler(FromClientHandler);
		HState = HostState::WaitMode;
		break;
	case HostState::WaitMode:
		if (Wifi_LibraryModeReady()) {
			Wifi_MultiplayerFromClientSetPacketHandler(FromClientHandler); // NDS-MP8: tras el cambio de modo
			Wifi_SetChannel(6);
			Wifi_MultiplayerAllowNewClients(true);
			Wifi_BeaconStart("DevilutionX", GameId);
			NetLog("anfitrion: listo, anunciando partida en canal 6");
			LastMask = 0;
			HState = HostState::Ready;
		}
		break;
	case HostState::Ready:
		PollHostMask();
		break;
	}
}

void EnsureWifi()
{
	if (nds_wifi_ok < 0) {
		nds_wifi_ok = Wifi_InitDefault(INIT_ONLY | WIFI_LOCAL_ONLY) ? 1 : 0;
		NetLog("wifi iniciada: %d (modo DSi %d)", nds_wifi_ok, isDSiMode() ? 1 : 0);
	}
}

void Service()
{
	if (InService)
		return;
	InService = true;
	{
		// NDS-MP17: avisar si el juego estuvo mucho rato sin atender la red (p.ej. cargando un nivel)
		static uint32_t lastEngine = 0;
		const uint32_t nowE = NowMs();
		if (!InNetThread && CurRole != Role::None && lastEngine != 0 && nowE - lastEngine > 500)
			NetLog("el juego estuvo %u ms sin atender la red", static_cast<unsigned>(nowE - lastEngine));
		if (!InNetThread)
			lastEngine = nowE;
	}
	static uint32_t lastStat = 0;
	if (CurRole != Role::None && NowMs() - lastStat >= 30000) { // NDS-MP18: cada 30 s (antes 2 s), menos escrituras en la SD
		lastStat = NowMs();
		NetLog("resumen: enviadas %u recibidas %u cmd env %u rec %u perdidas %u receptor %u (tipo %d len %d) assoc %d",
		    static_cast<unsigned>(StatTx), static_cast<unsigned>(StatRx), static_cast<unsigned>(StatCmdTx), static_cast<unsigned>(CmdArrived),
		    static_cast<unsigned>(RxDropped), static_cast<unsigned>(HandlerCalls), LastType, LastLen, Wifi_AssocStatus());
		// NDS-MP10: el ARM9 de la wifi trabaja desde la interrupcion de la FIFO: comprobar que esta activa
		{
			const unsigned ie = static_cast<unsigned>(REG_IE);
			NetLog("  IE %08X IME %u fifo-irq %s  TM0 %04X TM1 %04X TM2 %04X TM3 %04X/%04X", ie, static_cast<unsigned>(REG_IME),
			    (ie & IRQ_FIFO_NOT_EMPTY) ? "si" : "NO", static_cast<unsigned>(TIMER_CR(0)), static_cast<unsigned>(TIMER_CR(1)),
			    static_cast<unsigned>(TIMER_CR(2)), static_cast<unsigned>(TIMER_CR(3)), static_cast<unsigned>(TIMER_DATA(3))); // NDS-MP11
			if ((ie & IRQ_FIFO_NOT_EMPTY) == 0) {
				irqEnable(IRQ_FIFO_NOT_EMPTY);
				NetLog("  activada la interrupcion de la FIFO");
			}
		}
		// NDS-MP9: contadores internos de la libreria (Wifi_GetStats 0..19)
		NetLog("  stats %u %u %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u %u %u",
		    static_cast<unsigned>(Wifi_GetStats(0)), static_cast<unsigned>(Wifi_GetStats(1)), static_cast<unsigned>(Wifi_GetStats(2)),
		    static_cast<unsigned>(Wifi_GetStats(3)), static_cast<unsigned>(Wifi_GetStats(4)), static_cast<unsigned>(Wifi_GetStats(5)),
		    static_cast<unsigned>(Wifi_GetStats(6)), static_cast<unsigned>(Wifi_GetStats(7)), static_cast<unsigned>(Wifi_GetStats(8)),
		    static_cast<unsigned>(Wifi_GetStats(9)), static_cast<unsigned>(Wifi_GetStats(10)), static_cast<unsigned>(Wifi_GetStats(11)),
		    static_cast<unsigned>(Wifi_GetStats(12)), static_cast<unsigned>(Wifi_GetStats(13)), static_cast<unsigned>(Wifi_GetStats(14)),
		    static_cast<unsigned>(Wifi_GetStats(15)), static_cast<unsigned>(Wifi_GetStats(16)), static_cast<unsigned>(Wifi_GetStats(17)),
		    static_cast<unsigned>(Wifi_GetStats(18)), static_cast<unsigned>(Wifi_GetStats(19)));
	}
	if (CurRole == Role::Host)
		HostStep();
	else if (CurRole == Role::Client)
		ClientStep();
	if (CurRole != Role::None && nds_wifi_ok > 0) {
		for (int k = 0; k < 4; k++) // NDS-MP13: cada llamada reparte como mucho ~20 paquetes
			Wifi_Update();
	}
	while (RxRead != RxWrite) {
		RxSlot &s = RxRing[RxRead % RxSlots];
		HandleFrame(s.from, s.data, s.len);
		RxRead = RxRead + 1;
	}
	CheckGrace(); // NDS-MP14
	if (CurRole == Role::Host && HState == HostState::Ready)
		HostSendCmd();
	else if (CurRole == Role::Client && CState == ClientState::Connected)
		ClientPrepareReply();
	if (!InNetThread)
		FlushLog(); // NDS-MP16: solo el hilo del juego escribe en la SD
	InService = false;
}

// NDS-MP16: hilo propio de la red. Antes la red solo se atendia una vez por fotograma del juego
// (~20 por segundo, 1 trama de ida y 1 de vuelta), y eso daba los microcortes. Este hilo la
// atiende cada vez que el juego cede el procesador, como mucho cada 2 ms.
int NetThread(void *)
{
	uint32_t last = 0;
	for (;;) {
		if (CurRole != Role::None) {
			const uint32_t now = NowUs();
			if (now - last >= 2000) {
				last = now;
				InNetThread = true;
				Service();
				InNetThread = false;
			}
		}
		cothread_yield();
	}
	return 0;
}
bool NetThreadStarted = false;
// NDS-MP17: desactivado. No subio el ritmo de tramas (seguia en ~20 por segundo) y mantenia la radio
// trabajando mientras el juego carga archivos; en la DSi la SD y la wifi las atiende el mismo ARM7,
// y la DSi se colgo justo al entrar en la mazmorra.
constexpr bool UseNetThread = false;
void StartNetThread()
{
	if (NetThreadStarted || !UseNetThread)
		return;
	NetThreadStarted = true;
	cothread_create(NetThread, nullptr, 16 * 1024, COTHREAD_DETACHED);
}

void NdsNetHook()
{
	Service();
}

void SetRole(Role r)
{
	if (CurRole == r)
		return;
	EnsureWifi();
	NetLog("rol: %s", r == Role::Host ? "anfitrion" : (r == Role::Client ? "cliente" : "ninguno"));
	if (CurRole != Role::None)
		Wifi_IdleMode();
	for (uint8_t id = 0; id < NumIds; id++)
		ResetPeer(id);
	LostAt.fill(0); // NDS-MP14
	AwaitDrop.fill(false); // NDS-MP15
	MyTok = static_cast<uint16_t>(MyTok + 1 + (NowUs() & 0xFF)); // NDS-MP15: sesion nueva, distinta de la anterior
	if (MyTok == 0)
		MyTok = 1;
	Delivered.clear();
	OobQueue.clear();
	DisconnectQueue.clear();
	CurRole = r;
	CState = ClientState::Init;
	HState = HostState::Init;
	LastMask = 0;
	nds_net_hook = (r == Role::None) ? nullptr : NdsNetHook; // atendido tambien desde SDL_Flip/SDL_Delay
}

} // namespace

protocol_nds::protocol_nds()
{
	EnsureWifi();
	StartNetThread(); // NDS-MP16
}

protocol_nds::~protocol_nds()
{
	SetRole(Role::None);
	if (nds_wifi_ok > 0)
		Wifi_IdleMode();
}

void protocol_nds::prepare_host(bool host)
{
	SetRole(host ? Role::Host : Role::Client);
}

void protocol_nds::disconnect(const endpoint &peer)
{
	if (peer.id == 0 || peer.id >= NumIds)
		return;
	const bool linked = CurRole == Role::Host ? (LastMask & (1 << (peer.id - 1))) != 0
	                                          : (peer.id == 1 && CState == ClientState::Connected);
	ResetPeer(peer.id);
	Peers[peer.id].active = linked;
}

namespace {
// NDS-MP5: todo (tambien lo "fuera de banda") va por el canal fiable, troceado al tamano aceptado
void QueueReliable(uint8_t id, const buffer_t &data)
{
	// NDS-MP16: el mensaje va al flujo de salida con su largo delante; se trocea al enviar
	Peer &p = Peers[id];
	const size_t L = std::min<size_t>(data.size(), 0xFFFF);
	p.pending.push_back(static_cast<uint8_t>(L & 0xFF));
	p.pending.push_back(static_cast<uint8_t>(L >> 8));
	p.pending.insert(p.pending.end(), data.begin(), data.begin() + L);
}
} // namespace

std::expected<void, PacketError> protocol_nds::send(const endpoint &peer, const buffer_t &data)
{
	if (peer.id == 0 || peer.id >= NumIds || !Peers[peer.id].active)
		return std::unexpected(PacketError("DSi local: companero no conectado"));
	QueueReliable(peer.id, data);
	Service();
	return {};
}

bool protocol_nds::send_oob(const endpoint &peer, const buffer_t &data) const
{
	if (peer.id == 0 || peer.id >= NumIds || !Peers[peer.id].active)
		return false;
	if (++LogMsgOut <= 20 || LogMsgOut % 100 == 0)
		NetLog("(n%u) OOB a %u (%u bytes)", static_cast<unsigned>(LogMsgOut), peer.id, static_cast<unsigned>(data.size()));
	QueueReliable(peer.id, data);
	return true;
}

bool protocol_nds::send_oob_mc(const buffer_t &data) const
{
	if (CurRole == Role::Client) {
		if (CState != ClientState::Connected || !Peers[1].active)
			return false;
		// NDS-MP15: el juego pide la lista cada 10 ms mientras espera; con una cada 250 ms basta
		static uint32_t lastReq = 0;
		const uint32_t now = NowMs();
		if (lastReq != 0 && now - lastReq < 250)
			return true;
		lastReq = now;
		if (++LogReq <= 20 || LogReq % 100 == 0)
			NetLog("(n%u) cliente: peticion de partidas (%u bytes)", static_cast<unsigned>(LogReq), static_cast<unsigned>(data.size()));
		QueueReliable(1, data);
		return true;
	}
	if (CurRole == Role::Host) {
		for (uint8_t id = 2; id < NumIds; id++)
			if (Peers[id].active)
				QueueReliable(id, data);
		return true;
	}
	return false;
}

bool protocol_nds::recv(endpoint &peer, buffer_t &data)
{
	Service();
	if (!OobQueue.empty()) {
		peer.id = OobQueue.front().first;
		data = std::move(OobQueue.front().second);
		OobQueue.pop_front();
		return true;
	}
	if (!Delivered.empty()) {
		peer.id = Delivered.front().first;
		data = std::move(Delivered.front().second);
		Delivered.pop_front();
		return true;
	}
	return false;
}

bool protocol_nds::get_disconnected(endpoint &peer)
{
	if (DisconnectQueue.empty())
		return false;
	peer.id = DisconnectQueue.front();
	DisconnectQueue.pop_front();
	if (AwaitDrop[peer.id]) {
		// NDS-MP15: el juego ya se entera de la desconexion; el companero puede empezar de cero
		AwaitDrop[peer.id] = false;
		const bool linked = CurRole == Role::Host ? (LastMask & (1 << (peer.id - 1))) != 0
		                                          : (peer.id == 1 && CState == ClientState::Connected);
		if (linked)
			ActivatePeer(peer.id);
	}
	return true;
}

std::expected<bool, PacketError> protocol_nds::network_online()
{
	if (nds_wifi_ok == 0)
		return std::unexpected(PacketError("No se pudo iniciar la wifi"));
	if (CurRole == Role::None)
		SetRole(Role::Client);
	Service();
	if (CurRole == Role::Host)
		return HState == HostState::Ready;
	return CState == ClientState::Connected;
}

std::expected<bool, PacketError> protocol_nds::peers_ready()
{
	return network_online();
}

bool protocol_nds::is_peer_connected(endpoint &peer)
{
	return peer.id > 0 && peer.id < NumIds && Peers[peer.id].active;
}

std::string protocol_nds::make_default_gamename()
{
	static const char hex[] = "0123456789abcdef";
	const unsigned v = nds_usec();
	std::string name = "dsi-";
	for (int i = 0; i < 4; i++)
		name += hex[(v >> (i * 4 + 3)) & 0xF];
	return name;
}

} // namespace devilution::net
