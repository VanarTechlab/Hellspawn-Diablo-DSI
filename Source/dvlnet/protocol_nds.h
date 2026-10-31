// NDS-MP2: conector "DSi local" (multijugador DS a DS sin router) para base_protocol.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "dvlnet/packet.h"

namespace devilution::net {

class protocol_nds {
public:
	/** id 0 = nadie, 1 = anfitrion, 2..16 = cliente con AID (id - 1) */
	class endpoint {
	public:
		uint8_t id = 0;

		explicit operator bool() const { return id != 0; }
		bool operator==(const endpoint &rhs) const { return id == rhs.id; }
		bool operator!=(const endpoint &rhs) const { return id != rhs.id; }
		bool operator<(const endpoint &rhs) const { return id < rhs.id; }

		buffer_t serialize() const { return buffer_t(1, id); }

		std::expected<void, PacketError> unserialize(const buffer_t &buf)
		{
			if (buf.size() != 1)
				return std::unexpected(PacketError("DSi local: endpoint con tamano incorrecto"));
			id = buf[0];
			return {};
		}
	};

	protocol_nds();
	~protocol_nds();

	/** true: esta consola crea la partida (anfitrion); false: se une a una */
	void prepare_host(bool host);

	void disconnect(const endpoint &peer);
	std::expected<void, PacketError> send(const endpoint &peer, const buffer_t &data);
	bool send_oob(const endpoint &peer, const buffer_t &data) const;
	bool send_oob_mc(const buffer_t &data) const;
	bool recv(endpoint &peer, buffer_t &data);
	bool get_disconnected(endpoint &peer);
	std::expected<bool, PacketError> network_online();
	std::expected<bool, PacketError> peers_ready();
	bool is_peer_connected(endpoint &peer);
	std::optional<bool> is_peer_relayed(const endpoint &) const { return std::nullopt; }
	std::optional<int> get_latency_to(const endpoint &) const { return std::nullopt; }
	static std::string make_default_gamename();
};

} // namespace devilution::net
