#include "dvlnet/abstract_net.h"

#include "dvlnet/loopback.h"
#include "utils/stubs.h"

#ifndef NONET
#include "dvlnet/cdwrap.h"

#ifndef DISABLE_ZERO_TIER
#include "dvlnet/base_protocol.h"
#include "dvlnet/protocol_zt.h"
#endif

#ifndef DISABLE_TCP
#include "dvlnet/tcp_client.h"
#endif
#ifdef __NDS__ /* NDS-MP2 */
#include "dvlnet/base_protocol.h"
#include "dvlnet/protocol_nds.h"
#endif
#endif

namespace devilution::net {

std::unique_ptr<abstract_net> abstract_net::MakeNet(provider_t provider)
{
#ifdef NONET
	return std::make_unique<loopback>();
#else
	switch (provider) {
#ifdef __NDS__ /* NDS-MP2: "DSi local" ocupa el hueco de ZeroTier */
	case SELCONN_ZT:
		return std::make_unique<cdwrap>([]() {
			return std::make_unique<base_protocol<protocol_nds>>();
		});
#endif
#ifndef DISABLE_TCP
	case SELCONN_TCP:
		return std::make_unique<cdwrap>([]() {
			return std::make_unique<tcp_client>();
		});
#endif
#ifndef DISABLE_ZERO_TIER
	case SELCONN_ZT:
		return std::make_unique<cdwrap>([]() {
			return std::make_unique<base_protocol<protocol_zt>>();
		});
#endif
	case SELCONN_LOOPBACK:
		return std::make_unique<loopback>();
	default:
		ABORT();
	}
#endif
}

} // namespace devilution::net
