// Directory watcher with a debounce (S6: 300 ms). ReadDirectoryChangesW, one thread per directory plus a pump thread. No engine.
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace inkwell
{
	/// Collects changed paths and releases them once nothing new arrived for `quietMs`.
	class Debouncer
	{
	public:
		explicit Debouncer(long long quietMs) :
			_quiet(quietMs) {}
		void Add(const std::string& path, long long nowMs)
		{
			std::lock_guard lk(_m);
			_last = nowMs;
			_paths.insert(path);
		}
		/// The paths of a finished burst, or empty.
		std::set<std::string> Due(long long nowMs)
		{
			std::lock_guard lk(_m);
			if (_paths.empty() || nowMs - _last < _quiet) return {};
			return std::exchange(_paths, {});
		}

	private:
		std::mutex            _m;
		std::set<std::string> _paths;
		long long             _last{ 0 };
		long long             _quiet;
	};

	class DirWatcher
	{
	public:
		struct Dir
		{
			std::filesystem::path path;
			bool                  recursive{ false };
		};
		using Callback = std::function<void(const std::set<std::string>& changedPaths)>;  // runs on the pump thread

		DirWatcher() = default;
		~DirWatcher() { Stop(); }
		/// Directories that cannot be opened are skipped (listed in `skipped`). True if at least one is watched.
		bool Start(const std::vector<Dir>& dirs, long long quietMs, Callback cb, std::vector<std::string>& skipped);
		void Stop();

	private:
		struct State;
		std::shared_ptr<State>   _s;
		std::vector<std::thread> _threads;
	};
}
