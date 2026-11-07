#include "preview-output.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
namespace lbm {
bool play_preview_on_device(const std::vector<float> &, uint32_t, const std::string &, const std::atomic<bool> &,
			    std::string &, std::atomic<double> *);
}
int main(int argc, char **argv)
{
	if (argc != 4)
		return 2;
	const unsigned rate = std::stoul(argv[2]);
	const bool interrupt = std::string(argv[3]) == "cancel";
	std::vector<float> pcm(rate * 4 * 2);
	for (size_t i = 0; i < pcm.size() / 2; ++i)
		pcm[i * 2] = pcm[i * 2 + 1] = float(0.03 * std::sin(6.283185307179586 * 440 * i / rate));
	std::atomic<bool> cancel{false}, done{false};
	std::atomic<double> position{-1};
	std::string error;
	bool okay = false;
	std::thread worker([&] {
		try {
			okay = lbm::play_preview_on_device(pcm, rate, argv[1], cancel, error, &position);
		} catch (const std::exception &e) {
			error = e.what();
		}
		done.store(true);
	});
	double previous = -1, max_step = 0;
	bool intermediate = false, monotonic = true;
	unsigned updates = 0;
	while (!done.load()) {
		const double value = position.load();
		if (value >= 0) {
			monotonic = monotonic && value >= previous && value <= 4.0;
			if (value != previous) {
				if (previous >= 0 && value - previous > max_step)
					max_step = value - previous;
				++updates;
				std::cout << value << '\n';
			}
			previous = value;
			if (value > 0.1 && value < 3.9)
				intermediate = true;
			if (interrupt && value >= 1.0)
				cancel.store(true);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	worker.join();
	const double final = position.load();
	const bool passed = monotonic && intermediate && updates > 2 && error.empty() &&
			    (interrupt ? !okay && cancel.load() && final < 4.0 : okay && final == 4.0);
	std::cout << "passed=" << passed << " max_step=" << max_step << " final=" << final << " error=" << error
		  << '\n';
	return passed ? 0 : 1;
}
