﻿#include "inout.nogui.h"

#undef max
#include <algorithm>

const std::string header = "File name                                             +DLs      -DLs       I/O";

Output_PlainText::~Output_PlainText()
{
	interruptConsoleWriter = true;
	if (consoleWriter.joinable())
	{
		consoleWriter.join();
	}
}

Output_PlainText::Output_PlainText()
{
	consoleWriter = std::thread(&Output_PlainText::_consoleWriter, this);
}

void Output_PlainText::printToConsole(
	int colorPair, bool printTimestamp, bool leadingNewLine,
	bool trailingNewLine, bool fillLine, const wchar_t * format,
	...
)
{
	std::wstring message;
	SYSTEMTIME cur_time;
	GetLocalTime(&cur_time);
	wchar_t timeBuff[9];
	wchar_t timeBuffMil[13];
	swprintf(timeBuff, sizeof(timeBuff), L"%02d:%02d:%02d", cur_time.wHour, cur_time.wMinute, cur_time.wSecond);
	swprintf(timeBuffMil, sizeof(timeBuff), L"%02d:%02d:%02d.%03d", cur_time.wHour, cur_time.wMinute, cur_time.wSecond, cur_time.wMilliseconds);
	std::wstring time = timeBuff;
	std::wstring timeMil = timeBuffMil;

	if (printTimestamp) {

		message = timeBuff;
		message += L" ";
	}

	va_list args;
	va_start(args, format);
	int size = vswprintf(nullptr, 0, format, args) + 1;
	va_end(args);
	std::unique_ptr<wchar_t[]> buf(new wchar_t[size]);
	va_list args2;
	va_start(args2, format);
	vswprintf(buf.get(), size, format, args2);
	va_end(args2);
	message += std::wstring(buf.get(), buf.get() + size - 1);
	{
		std::lock_guard<std::mutex> lockGuard(mConsoleQueue);
		consoleQueue.push_back({
			//colorPair,
			leadingNewLine, trailingNewLine,
			fillLine,
			message });
	}
	{
		std::lock_guard<std::mutex> lockGuard(mLog);
		loggingQueue.push_back(timeMil + L" " + std::wstring(buf.get(), buf.get() + size - 1));
	}
}

void Output_PlainText::printHeadlineTitle(
	std::wstring const& appname,
	std::wstring const& version,
	bool elevated
)
{
	printToConsole(12, false, false, true, false, L"%s, %s %s",
		appname.c_str(), version.c_str(),
		elevated ? L"(elevated)" : L"(nonelevated)");
}

void Output_PlainText::printWallOfCredits(
	std::vector<std::wstring> const& history
)
{
	for (auto&& item : history)
		printToConsole(4, false, false, true, false, item.c_str());
}

void Output_PlainText::printHWInfo(
	hwinfo const& info
)
{
	printToConsole(-1, false, true, false, false, L"CPU support: ");
	if (info.AES)		printToConsole(-1, false, false, false, false, L" AES ");
	if (info.SSE)		printToConsole(-1, false, false, false, false, L" SSE ");
	if (info.SSE2)		printToConsole(-1, false, false, false, false, L" SSE2 ");
	if (info.SSE3)		printToConsole(-1, false, false, false, false, L" SSE3 ");
	if (info.SSE42)		printToConsole(-1, false, false, false, false, L" SSE4.2 ");
	if (info.AVX)		printToConsole(-1, false, false, false, false, L" AVX ");
	if (info.AVX2)		printToConsole(-1, false, false, false, false, L" AVX2 ");
	if (info.AVX512F)	printToConsole(-1, false, false, false, false, L" AVX512F ");

	if (info.avxsupported)	printToConsole(-1, false, false, false, false, L"     [recomend use AVX]");
	if (info.AVX2)			printToConsole(-1, false, false, false, false, L"     [recomend use AVX2]");
	if (info.AVX512F)		printToConsole(-1, false, false, false, false, L"     [recomend use AVX512F]");

	printToConsole(-1, false, true, false, false, L"%S %S [%u cores]", info.vendor.c_str(), info.brand.c_str(), info.cores);
	printToConsole(-1, false, true, true, false, L"RAM: %llu Mb", info.memory);
}

void Output_PlainText::printPlotsStart()
{
	printToConsole(15, false, false, true, false, L"Using plots:");
}

void Output_PlainText::printPlotsInfo(char const* const directory, unsigned nfiles, unsigned long long size)
{
	printToConsole(-1, false, false, true, false, L"%S\tfiles: %4u\t size: %7llu GiB",
		directory, nfiles, size / 1024 / 1024 / 1024);
}

void Output_PlainText::printPlotsEnd(unsigned long long total_size)
{
	printToConsole(15, false, false, true, false, L"TOTAL: %llu GiB (%llu TiB)",
		total_size / 1024 / 1024 / 1024, total_size / 1024 / 1024 / 1024 / 1024);
}

void Output_PlainText::printThreadActivity(
	std::wstring const& coinName,
	std::wstring const& threadKind,
	std::wstring const& threadAction
)
{
	printToConsole(25, false, false, true, true, L"%s %s thread %s", coinName.c_str(), threadKind.c_str(), threadAction.c_str());
}

void Output_PlainText::debugWorkerStats(
	std::wstring const& specialReadMode,
	std::string const& directory,
	double proc_time, double work_time,
	unsigned long long files_size_per_thread
)
{
	auto msgFormat = !specialReadMode.empty()
		? L"Thread \"%S\" @ %.1f sec (%.1f MB/s) CPU %.2f%% (%s)"
		: L"Thread \"%S\" @ %.1f sec (%.1f MB/s) CPU %.2f%%%s"; // note that the last %s is always empty, it is there just to keep the same number of placeholders

	printToConsole(7, true, false, true, false, msgFormat,
		directory.c_str(), work_time, (double)(files_size_per_thread) / work_time / 1024 / 1024 / 4096,
		proc_time / work_time * 100);
}

void Output_PlainText::printWorkerDeadlineFound(
	unsigned long long account_id,
	std::wstring const& coinName,
	unsigned long long deadline
)
{
	printToConsole(2, true, false, true, false, L"[%20llu|%-10s|Worker] DL found     : %11llu",
		account_id, coinName.c_str(),
		deadline);
}

void Output_PlainText::printNetworkHostResolution(
	std::wstring const& lookupWhat,
	std::wstring const& coinName,
	std::string const& remoteName,
	std::vector<char> const& resolvedIP,
	std::string const& remotePost,
	std::string const& remotePath
)
{
	printToConsole(-1, false, false, true, false, L"%s %15s %S (ip %S:%S) %S", coinName.c_str(), lookupWhat.c_str(),
		remoteName.c_str(), resolvedIP.data(), remotePost.c_str(), ((remotePath.length() ? "on /" : "") + remotePath).c_str());
}

void Output_PlainText::printNetworkProxyDeadlineReceived(
	unsigned long long account_id,
	std::wstring const& coinName,
	unsigned long long deadline,
	char const (& const clientAddr)[22]
)
{
	printToConsole(2, true, false, true, false, L"[%20llu|%-10s|Proxy ] DL found     : %11llu {%S}",
		account_id, coinName.c_str(),
		deadline, clientAddr);
}

void Output_PlainText::debugNetworkProxyDeadlineAcked(
	unsigned long long account_id,
	std::wstring const& coinName,
	unsigned long long deadline,
	char const (& const clientAddr)[22]
)
{
	printToConsole(9, true, false, true, false, L"[%20llu|%-10s|Proxy ] DL ack'ed    : %11llu {%S}",
		account_id, coinName.c_str(),
		deadline, clientAddr);
}

void Output_PlainText::debugNetworkDeadlineDiscarded(
	unsigned long long account_id,
	std::wstring const& coinName,
	unsigned long long deadline,
	unsigned long long targetDeadline
)
{
	printToConsole(2, true, false, true, false, L"[%20llu|%-10s|Sender] DL discarded : %11llu > %11llu",
		account_id, coinName.c_str(),
		deadline, targetDeadline);
}

void Output_PlainText::printNetworkDeadlineSent(
	unsigned long long account_id,
	std::wstring const& coinName,
	unsigned long long deadline
)
{
	unsigned long long days = (deadline) / (24 * 60 * 60);
	unsigned hours = (deadline % (24 * 60 * 60)) / (60 * 60);
	unsigned min = (deadline % (60 * 60)) / 60;
	unsigned sec = deadline % 60;

	printToConsole(10, true, false, true, false, L"[%20llu|%-10s|Sender] DL sent      : %11llu %7ud %02u:%02u:%02u",
		account_id, coinName.c_str(),
		deadline,
		days, hours, min, sec);
}

void Output_PlainText::printNetworkDeadlineConfirmed(
	bool with_timespan,
	unsigned long long account_id,
	std::wstring const& coinName,
	unsigned long long deadline
)
{
	if (!with_timespan)
	{
		printToConsole(10, true, false, true, false, L"[%20llu|%-10s|Sender] DL confirmed : %s",
			account_id, coinName.c_str(),
			deadline
		);
	}
	else
	{
		unsigned long long days = (deadline) / (24 * 60 * 60);
		unsigned hours = (deadline % (24 * 60 * 60)) / (60 * 60);
		unsigned min = (deadline % (60 * 60)) / 60;
		unsigned sec = deadline % 60;

		printToConsole(10, true, false, true, false, L"[%20llu|%-10s|Sender] DL confirmed : %11llu %7ud %02u:%02u:%02u",
			account_id, coinName.c_str(),
			deadline,
			days, hours, min, sec);
	}
}

void Output_PlainText::debugNetworkTargetDeadlineUpdated(
	unsigned long long account_id,
	std::wstring const& coinName,
	unsigned long long targetDeadline
)
{
	printToConsole(10, true, false, true, false, L"[%20llu|%-10s|Sender] Set target DL: %11llu",
		account_id, coinName.c_str(),
		targetDeadline);
}

void Output_PlainText::debugRoundTime(
	double threads_time
)
{
	printToConsole(7, true, false, true, false, L"Total round time: %.1f sec", threads_time);
}

void Output_PlainText::printBlockEnqueued(
	unsigned long long currentHeight,
	std::wstring const& coinName,
	bool atEnd, bool noQueue
)
{
	if (noQueue)
		printToConsole(5, true, false, true, false, L"[#%7llu|%-10s|Info    ] New block.",
			currentHeight, coinName.c_str());
	else
		printToConsole(5, true, false, true, false, L"[#%7llu|%-10s|Info    ] New block has been added to the%s queue.",
			currentHeight, coinName.c_str(), atEnd ? L" end of the" : L"");
}

void Output_PlainText::printRoundInterrupt(
	unsigned long long currentHeight,
	std::wstring const& coinName
)
{
	printToConsole(8, true, false, true, true, L"[#%7llu|%-10s|Info    ] Mining has been interrupted by another coin.",
		currentHeight, coinName.c_str());
}

void Output_PlainText::printRoundChangeInfo(bool isResumingInterrupted,
	unsigned long long currentHeight,
	std::wstring const& coinName,
	unsigned long long currentBaseTarget,
	unsigned long long currentNetDiff,
	bool isPoc2Round
)
{
	auto msgFormat = isResumingInterrupted
		? L"[#%7llu|%-10s|Continue] Base Target %7llu %c Net Diff %8llu TiB %c PoC%i"
		: L"[#%7llu|%-10s|Start   ] Base Target %7llu %c Net Diff %8llu TiB %c PoC%i";

	auto colorPair = isResumingInterrupted
		? 5
		: 25;

	printToConsole(colorPair, true, false, true, false, msgFormat,
		currentHeight, coinName.c_str(),
		currentBaseTarget, sepChar,
		currentNetDiff, sepChar,
		isPoc2Round ? 2 : 1);
}

// TODO: this proc is called VERY often; for now, we want to skip that
// in future it should emit messages via printToConsole, but it should be
// throttled in some way
// also, to be able to guarantee that end-of-round 100% will be displayed
// the parameters must be changed from format+... to specific data
void Output_PlainText::printConnQuality(int ncoins, std::wstring const& connQualInfo)
{
	if (ncoins != prevNCoins94)
	{
		std::lock_guard<std::mutex> lockGuard(mConsoleQueue);
		if (ncoins != prevNCoins94)
			leadingSpace94 = IUserInterface::make_leftpad_for_networkstats(94, ncoins);
	}

	size_t size = swprintf(nullptr, 0, L"%s%s", leadingSpace94.c_str(), connQualInfo.c_str()) + 1;
	std::unique_ptr<wchar_t[]> buf(new wchar_t[size]);
	swprintf(buf.get(), size, L"%s%s", leadingSpace94.c_str(), connQualInfo.c_str());

	auto message = std::wstring(buf.get(), buf.get() + size - 1);
	{
		std::lock_guard<std::mutex> lockGuard(mConsoleQueue);
		consoleQueue.push_back({
			false, true,
			false,
			message });
	}
}

void Output_PlainText::printScanProgress(int ncoins, std::wstring const& connQualInfo,
	unsigned long long bytesRead, unsigned long long round_size,
	double thread_time, double threads_speed,
	unsigned long long deadline
)
{
	if (ncoins != prevNCoins21)
	{
		std::lock_guard<std::mutex> lockGuard(mConsoleQueue);
		if (ncoins != prevNCoins21)
			leadingSpace21 = IUserInterface::make_leftpad_for_networkstats(21, ncoins);
	}

	size_t size = swprintf(nullptr, 0, L"%3llu%% %c %11.2f TiB %c %4.0f s %c %6.0f MiB/s %c Deadline: %s %c %s%s",
		(bytesRead * 4096 * 100 / std::max(1ull, round_size)), sepChar,
		(((double)bytesRead) / (256 * 1024 * 1024)), sepChar,
		thread_time, sepChar,
		threads_speed, sepChar,
		(deadline == 0) ? L"          -" : toWStr(deadline, 11).c_str(), sepChar,
		leadingSpace94.c_str(), connQualInfo.c_str()) + 1;
	std::unique_ptr<wchar_t[]> buf(new wchar_t[size]);
	swprintf(buf.get(), size, L"%3llu%% %c %11.2f TiB %c %4.0f s %c %6.0f MiB/s %c Deadline: %s %c %s%s",
		(bytesRead * 4096 * 100 / std::max(1ull, round_size)), sepChar,
		(((double)bytesRead) / (256 * 1024 * 1024)), sepChar,
		thread_time, sepChar,
		threads_speed, sepChar,
		(deadline == 0) ? L"          -" : toWStr(deadline, 11).c_str(), sepChar,
		leadingSpace21.c_str(), connQualInfo.c_str());

	auto message = std::wstring(buf.get(), buf.get() + size - 1);
	{
		std::lock_guard<std::mutex> lockGuard(mConsoleQueue);
		consoleQueue.push_back({
			false, true,
			false,
			message });
	}
}

void Output_PlainText::_consoleWriter() {
	while (!interruptConsoleWriter) {
		if (!consoleQueue.empty()) {
			ConsoleOutput consoleOutput;
			bool skip = false;
			
			{
				std::lock_guard<std::mutex> lockGuard(mConsoleQueue);
				if (consoleQueue.size() == 1 && consoleQueue.front().message == L"\n") {
					skip = true;
				}
			}

			if (skip) {
				std::this_thread::yield();
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			} 
			else {
				std::lock_guard<std::mutex> lockGuard(mConsoleQueue);
				consoleOutput = consoleQueue.front();
				consoleQueue.pop_front();
			}
			
			if (consoleOutput.leadingNewLine) {
				wprintf(L"\n");
			}
			wprintf(consoleOutput.message.c_str());
			if (consoleOutput.trailingNewLine) {
				wprintf(L"\n");
			}
		}
		else {
			std::this_thread::yield();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
}

bool Output_PlainText::currentlyDisplayingCorruptedPlotFiles() {
	return false;
}

int Output_PlainText::bm_wgetchMain() {
	// TODO: maybe somehow read nonblocking lines from CIN?
	// are we even interested in receiving any control commands other than KILL?
	return -1;
}

void Output_PlainText::showNewVersion(std::string version) {
	std::string tmp = "New version available: " + version + "\n";
	printToConsole(0, false, true, true, false, std::wstring(tmp.begin(), tmp.end()).c_str());
}

void Output_PlainText::printFileStats(std::map<std::string, t_file_stats>const& fileStats) {
	int lineCount = 0;
	for (auto& element : fileStats)
		if (element.second.conflictingDeadlines > 0 || element.second.readErrors > 0)
			++lineCount;

	if (lineCount == 0)
		return;

	std::string tmp = header;
	tmp += "\n";

	for (auto& element : fileStats)
		if (element.second.conflictingDeadlines > 0 || element.second.readErrors > 0) {
			tmp += toStr(element.first, 46);
			tmp += " ";
			tmp += toStr(element.second.matchingDeadlines, 11);
			//if (element.second.conflictingDeadlines > 0) {
			//	bm_wattronC(4);
			//}
			tmp += " ";
			tmp += toStr(element.second.conflictingDeadlines, 9);
			//if (element.second.readErrors > 0) {
			//	bm_wattronC(4);
			//}
			tmp += " ";
			tmp += toStr(element.second.readErrors, 9);
			tmp += "\n";
		}

	printToConsole(0, false, true, true, false, std::wstring(tmp.begin(), tmp.end()).c_str());
}
