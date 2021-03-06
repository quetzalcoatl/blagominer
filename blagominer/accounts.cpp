#include "stdafx.h"
#include "accounts.h"

size_t Get_index_acc(unsigned long long const key, std::shared_ptr<t_coin_info> coin)
{
	// locks->mTargetDeadlineInfo :=> mining->targetDeadlineInfo
	unsigned long long const targetDeadlineInfo = getTargetDeadlineInfo(coin);

	std::lock_guard<std::mutex> lockGuard(coin->locks->bestsLock);
	size_t acc_index = 0;
	for (auto it = coin->mining->bests.begin(); it != coin->mining->bests.end(); ++it)
	{
		if (it->account_id == key)
		{
			return acc_index;
		}
		acc_index++;
	}

	coin->mining->bests.push_back({ key, ULLONG_MAX, 0, 0, targetDeadlineInfo });
	return coin->mining->bests.size() - 1;
}
