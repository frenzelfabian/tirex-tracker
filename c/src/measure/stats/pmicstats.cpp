#include "pmicstats.hpp"

#include "../../logging.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

using tirex::PmicReader;

#if defined(__linux__)
std::optional<PmicReader::Sample> PmicReader::readOnce() {
	// `vcgencmd pmic_read_adc` prints one line per rail, e.g.:
	//   VDD_CORE_A current(7)=1.89316000A
	//   VDD_CORE_V volt(15)=0.91142770V
	// We collect current (suffix _A) and voltage (suffix _V) per rail and form the power U*I.
	FILE* pipe = popen("vcgencmd pmic_read_adc 2>/dev/null", "r");
	if (!pipe)
		return std::nullopt;

	std::map<std::string, double> amp, volt;
	char line[256];
	while (fgets(line, sizeof(line), pipe)) {
		std::string s(line);
		const auto eq = s.find('=');
		if (eq == std::string::npos)
			continue;
		const auto begin = s.find_first_not_of(" \t");
		if (begin == std::string::npos)
			continue;
		const auto ws = s.find_first_of(" \t", begin);
		if (ws == std::string::npos || ws > eq)
			continue;
		const std::string token = s.substr(begin, ws - begin); // e.g. "VDD_CORE_A"
		if (token.size() < 3 || token[token.size() - 2] != '_')
			continue;
		const char kind = token.back();
		if (kind != 'A' && kind != 'V')
			continue;
		const std::string rail = token.substr(0, token.size() - 2); // strip the trailing "_A"/"_V"
		const double value = std::strtod(s.c_str() + eq + 1, nullptr); // stops at the trailing unit char
		(kind == 'A' ? amp : volt)[rail] = value;
	}

	const int rc = pclose(pipe);
	if (rc != 0 || (amp.empty() && volt.empty()))
		return std::nullopt;

	const auto power = [&](const char* rail) -> double {
		const auto a = amp.find(rail);
		const auto v = volt.find(rail);
		return (a == amp.end() || v == volt.end()) ? 0.0 : a->second * v->second;
	};

	Sample sample{};
	sample.coreW = power("VDD_CORE");
	sample.ramW = power("DDR_VDD2") + power("DDR_VDDQ");
	return sample;
}

bool PmicReader::available() {
	static const bool avail = []() {
		const auto sample = readOnce();
		const bool ok = sample.has_value() && (sample->coreW > 0.0 || sample->ramW > 0.0);
		if (ok)
			tirex::log::info("pmic", "Raspberry Pi PMIC detected; using it for CPU/RAM energy");
		return ok;
	}();
	return avail;
}
#else
std::optional<PmicReader::Sample> PmicReader::readOnce() { return std::nullopt; }
bool PmicReader::available() { return false; }
#endif

void PmicReader::start() {
	coreJ = ramJ = 0.0;
	hasPrev = false;
	step(); // seed the first sample
}

void PmicReader::step() {
	const auto sample = readOnce();
	const auto now = std::chrono::steady_clock::now();
	if (sample && hasPrev) {
		const double dt = std::chrono::duration<double>(now - prevTime).count();
		coreJ += 0.5 * (prev.coreW + sample->coreW) * dt;
		ramJ += 0.5 * (prev.ramW + sample->ramW) * dt;
	}
	if (sample) {
		prev = *sample;
		prevTime = now;
		hasPrev = true;
	}
}

void PmicReader::stop() { step(); }
