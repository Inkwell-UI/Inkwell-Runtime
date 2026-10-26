#include "DirWatcher.h"

#include <atomic>
#include <chrono>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace inkwell
{
	struct DirWatcher::State
	{
		std::atomic<bool>          stop{ false };
		HANDLE                     stopEvent{ nullptr };
		std::unique_ptr<Debouncer> deb;
		Callback                   cb;
	};

	namespace
	{
		long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
	}

	bool DirWatcher::Start(const std::vector<Dir>& dirs, long long quietMs, Callback cb, std::vector<std::string>& skipped)
	{
		Stop();
		_s = std::make_shared<State>();
		_s->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		_s->deb = std::make_unique<Debouncer>(quietMs);
		_s->cb = std::move(cb);
		int started = 0;
		for (const auto& d : dirs) {
			HANDLE h = CreateFileW(d.path.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				skipped.push_back(d.path.string());
				continue;
			}
			++started;
			_threads.emplace_back([st = _s, h, d] {
				alignas(DWORD) char buf[64 * 1024];
				OVERLAPPED          ov{};
				ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
				const HANDLE waits[2] = { ov.hEvent, st->stopEvent };
				while (!st->stop) {
					ResetEvent(ov.hEvent);
					DWORD got = 0;
					if (!ReadDirectoryChangesW(h, buf, sizeof(buf), d.recursive ? TRUE : FALSE,
							FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION, &got, &ov, nullptr))
						break;
					if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
						CancelIoEx(h, &ov);
						GetOverlappedResult(h, &ov, &got, TRUE);
						break;
					}
					if (!GetOverlappedResult(h, &ov, &got, FALSE)) break;
					if (got == 0) {  // buffer overflow: something changed, we do not know what
						st->deb->Add(d.path.string(), NowMs());
						continue;
					}
					for (char* p = buf;;) {
						const auto*  fi = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(p);
						std::wstring name(fi->FileName, fi->FileNameLength / sizeof(WCHAR));
						st->deb->Add((d.path / name).string(), NowMs());
						if (fi->NextEntryOffset == 0) break;
						p += fi->NextEntryOffset;
					}
				}
				CloseHandle(ov.hEvent);
				CloseHandle(h);
			});
		}
		_threads.emplace_back([st = _s] {  // pump
			while (!st->stop) {
				WaitForSingleObject(st->stopEvent, 50);
				if (st->stop) break;
				auto due = st->deb->Due(NowMs());
				if (!due.empty()) st->cb(due);
			}
		});
		return started > 0;
	}

	void DirWatcher::Stop()
	{
		if (!_s) return;
		_s->stop = true;
		SetEvent(_s->stopEvent);
		for (auto& t : _threads) {
			if (t.joinable()) t.join();
		}
		_threads.clear();
		CloseHandle(_s->stopEvent);
		_s.reset();
	}
}
