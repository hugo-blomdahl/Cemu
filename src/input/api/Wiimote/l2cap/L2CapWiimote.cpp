#include "L2CapWiimote.h"
#include "config/ActiveSettings.h"
#include <bluetooth/l2cap.h>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>

constexpr auto comparator = [](const bdaddr_t& a, const bdaddr_t& b) {
	return bacmp(&a, &b);
};

static auto s_addresses = std::map<bdaddr_t, bool, decltype(comparator)>(comparator);
static std::mutex s_addressMutex;

static void LoadSavedAddresses()
{
	static bool loaded = false;
	if (loaded)
		return;
	loaded = true;

	std::error_code ec;
	const auto filePath = ActiveSettings::GetConfigPath("wiimotes.txt");
	if (!fs::exists(filePath, ec))
		return;

	std::ifstream f(filePath);
	std::string line;
	while (std::getline(f, line))
	{
		bdaddr_t addr{};
		int b[6];
		if (sscanf(line.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &b[5], &b[4], &b[3], &b[2], &b[1], &b[0]) == 6)
		{
			for (int i = 0; i < 6; ++i)
				addr.b[i] = (uint8)b[i];
			s_addresses.try_emplace(addr, false);
			cemuLog_log(LogType::Force, "Loaded saved Wiimote address: {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
						b[5], b[4], b[3], b[2], b[1], b[0]);
		}
	}
}

static void SaveAddresses()
{
	const auto filePath = ActiveSettings::GetConfigPath("wiimotes.txt");
	std::ofstream f(filePath, std::ios::trunc);
	for (const auto& [addr, _] : s_addresses)
	{
		const auto& b = addr.b;
		f << fmt::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}\n", b[5], b[4], b[3], b[2], b[1], b[0]);
	}
}

static bool AttemptConnect(int sockFd, const sockaddr_l2& addr)
{
	auto res = connect(sockFd, reinterpret_cast<const sockaddr*>(&addr),
					   sizeof(sockaddr_l2));
	if (res == 0)
		return true;
	return connect(sockFd, reinterpret_cast<const sockaddr*>(&addr),
				   sizeof(sockaddr_l2)) == 0;
}

static bool AttemptSetNonBlock(int sockFd)
{
	return fcntl(sockFd, F_SETFL, fcntl(sockFd, F_GETFL) | O_NONBLOCK) == 0;
}

L2CapWiimote::L2CapWiimote(int controlFd, int dataFd, bdaddr_t addr)
	: m_controlFd(controlFd), m_dataFd(dataFd), m_addr(addr)
{
}

L2CapWiimote::~L2CapWiimote()
{
	close(m_dataFd);
	close(m_controlFd);
	const auto& b = m_addr.b;
	cemuLog_logDebug(LogType::Force, "Wiimote at {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} disconnected", b[5], b[4], b[3], b[2], b[1], b[0]);

	// Re-add to candidate vec
	s_addressMutex.lock();
	s_addresses[m_addr] = false;
	s_addressMutex.unlock();
}

void L2CapWiimote::AddCandidateAddress(bdaddr_t addr)
{
	std::scoped_lock lock(s_addressMutex);
	LoadSavedAddresses();
	s_addresses.try_emplace(addr, false);
	SaveAddresses();
}

std::vector<WiimoteDevicePtr> L2CapWiimote::get_devices()
{
	s_addressMutex.lock();
	LoadSavedAddresses();
	std::vector<bdaddr_t> unconnected;
	for (const auto& [addr, connected] : s_addresses)
	{
		if (!connected)
			unconnected.push_back(addr);
	}
	s_addressMutex.unlock();

	std::vector<WiimoteDevicePtr> outDevices;
	for (const auto& addr : unconnected)
	{
		// Control socket, PSM 0x11, needs to be open for the data socket to be opened
		auto controlFd = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
		if (controlFd < 0)
		{
			cemuLog_logDebug(LogType::Force, "Failed to open control socket: {}", strerror(errno));
			continue;
		}

		sockaddr_l2 controlAddr{};
		controlAddr.l2_family = AF_BLUETOOTH;
		controlAddr.l2_psm = htobs(0x11);
		controlAddr.l2_bdaddr = addr;

		if (!AttemptConnect(controlFd, controlAddr) || !AttemptSetNonBlock(controlFd))
		{
			const auto& b = addr.b;
			cemuLog_logDebug(LogType::Force, "Failed to connect control socket to '{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}': {}",
							 b[5], b[4], b[3], b[2], b[1], b[0], strerror(errno));
			close(controlFd);
			continue;
		}

		// Socket for sending and receiving data from controller, PSM 0x13
		auto dataFd = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
		if (dataFd < 0)
		{
			cemuLog_logDebug(LogType::Force, "Failed to open data socket: {}", strerror(errno));
			close(controlFd);
			continue;
		}
		sockaddr_l2 dataAddr{};
		dataAddr.l2_family = AF_BLUETOOTH;
		dataAddr.l2_psm = htobs(0x13);
		dataAddr.l2_bdaddr = addr;

		if (!AttemptConnect(dataFd, dataAddr) || !AttemptSetNonBlock(dataFd))
		{
			const auto& b = addr.b;
			cemuLog_logDebug(LogType::Force, "Failed to connect data socket to '{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}': {}",
							 b[5], b[4], b[3], b[2], b[1], b[0], strerror(errno));
			close(dataFd);
			close(controlFd);
			continue;
		}
		const auto& b = addr.b;
		cemuLog_log(LogType::Force, "Connected to Wiimote/BalanceBoard at {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
					b[5], b[4], b[3], b[2], b[1], b[0]);
		outDevices.emplace_back(std::make_shared<L2CapWiimote>(controlFd, dataFd, addr));

		s_addressMutex.lock();
		s_addresses[addr] = true;
		s_addressMutex.unlock();
	}
	return outDevices;
}

bool L2CapWiimote::write_data(const std::vector<uint8>& data)
{
	const auto size = data.size();
	cemu_assert_debug(size < 23);
	uint8 buffer[23];
	// All outgoing messages must be prefixed with 0xA2
	buffer[0] = 0xA2;
	std::memcpy(buffer + 1, data.data(), size);
	const auto outSize = size + 1;
	return send(m_dataFd, buffer, outSize, 0) == outSize;
}

std::optional<std::vector<uint8>> L2CapWiimote::read_data()
{
	uint8 buffer[23];
	const auto nBytes = recv(m_dataFd, buffer, 23, 0);

	if (nBytes < 0 && errno == EWOULDBLOCK)
		return std::vector<uint8>{};
	// All incoming messages must be prefixed with 0xA1
	if (nBytes < 2 || buffer[0] != 0xA1)
		return std::nullopt;
	return std::vector(buffer + 1, buffer + 1 + nBytes - 1);
}

bool L2CapWiimote::operator==(const WiimoteDevice& rhs) const
{
	auto mote = dynamic_cast<const L2CapWiimote*>(&rhs);
	if (!mote)
		return false;
	return bacmp(&m_addr, &mote->m_addr) == 0;
}
