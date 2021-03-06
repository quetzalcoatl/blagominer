#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "reference/burst/BurstMath.h"
#include "reference/diskcoin/DiskcoinMath.h"
#include "hexstring.h"


//+ sample data from felixbrucker: testing if various 'burstmath' modules produce the same results

TEST(RnD_BurstMath_CalcDeadline_Felix, unk523458) {
	auto const height = 523458;
	uint64_t account_nr = 14541460361080926540;
	uint64_t nonce_nr = 804427435;
	uint32_t scoop_nr = 1057;

	uint64_t currentBaseTarget = 74102;
	auto currentSignature = HexString::arrayfrom<32>("6d0be5b1bf78cab7e37a5573af0074bcc4a885c456d433a2b063f854ba2e422f");

	unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);

	EXPECT_EQ(deadline, 441);
}

TEST(RnD_BurstMath_CalcDeadline_Felix, unk523459) {
	auto const height = 523459;
	uint64_t account_nr = 12389242946914536976;
	uint64_t nonce_nr = 4026905118;
	uint32_t scoop_nr = 3550;

	uint64_t currentBaseTarget = 75275;
	auto currentSignature = HexString::arrayfrom<32>("7396ad4f201a924215cd21ae227c42d87bbe704fcf04b26d48ec99773ce0dacb");

	unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);

	EXPECT_EQ(deadline, 180);
}

TEST(RnD_BurstMath_CalcDeadline_Felix, unk523460) {
	auto const height = 523460;
	uint64_t account_nr = 5838076873831982979;
	uint64_t nonce_nr = 375474931;
	uint32_t scoop_nr = 1204;

	uint64_t currentBaseTarget = 77583;
	auto currentSignature = HexString::arrayfrom<32>("18d32dc0c20abba377e544264080e597064485a57ea510e1c9f03bbd00387d54");

	unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);

	EXPECT_EQ(deadline, 536);
}

//- sample data from felixbrucker

//+ diskcoin experiments, data from felixbrucker

TEST(RnD_BurstMath_CalcDeadline_Felix, dcc006479differsFromClassicBurst) {
	auto const height = 6479;
	auto currentSignature = HexString::arrayfrom<32>("b01f8da00aa5f6eaf801b2265c9931ecc03216ea2f62608d35df749fcfe3caad");
	uint64_t currentBaseTarget = 41866166;
	uint64_t account_nr = 1723641494112555138;

	{
		uint64_t nonce_nr = 988;
		uint32_t scoop_nr = BurstMath::calculate_scoop(height, *currentSignature); // 3608
		unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);
		EXPECT_NE(deadline, /*dcminer:*/12696127); //burst:404211863135
	}
	{
		uint64_t nonce_nr = 44459;
		uint32_t scoop_nr = BurstMath::calculate_scoop(height, *currentSignature); // 3608
		unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);
		EXPECT_NE(deadline, /*dcminer:*/9596714); // burst:365328644164
	}
}

TEST(RnD_BurstMath_CalcDeadline_Felix, dcc006493differsFromClassicBurst) {
	auto const height = 6493;
	auto currentSignature = HexString::arrayfrom<32>("c6e466a394ebfeed0f137e408a2fd61224860c12301c72c11a5c68d3d3a14f55");
	uint64_t currentBaseTarget = 30090363;
	uint64_t account_nr = 1723641494112555138;

	{
		uint64_t nonce_nr = 8540;
		uint32_t scoop_nr = BurstMath::calculate_scoop(height, *currentSignature); // 1940
		unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);
		EXPECT_NE(deadline, /*dcminer:*/2534226); // burst:608875396419
	}
	{
		uint64_t nonce_nr = 19631;
		uint32_t scoop_nr = BurstMath::calculate_scoop(height, *currentSignature); // 1940
		unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);
		EXPECT_NE(deadline, /*dcminer:*/1216177); // burst:587876528901
	}
}

TEST(RnD_BurstMath_CalcDeadline_Felix, dcc006548differsFromClassicBurst) {
	auto const height = 6548;
	auto currentSignature = HexString::arrayfrom<32>("30acd4cf4f2abb14e052b2ee79684da33ec1838d68af89171f63695e6ce43eea");
	uint64_t currentBaseTarget = 28823010;
	uint64_t account_nr = 1723641494112555138;

	{
		uint64_t nonce_nr = 54537;
		uint32_t scoop_nr = BurstMath::calculate_scoop(height, *currentSignature); // 2651
		unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);
		EXPECT_NE(deadline, /*dcminer:*/22825302); // burst:59863254207
	}
	{
		uint64_t nonce_nr = 58049;
		uint32_t scoop_nr = BurstMath::calculate_scoop(height, *currentSignature); // 2651
		unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);
		EXPECT_NE(deadline, /*dcminer:*/17892974); // burst:45378756064
	}
}

TEST(RnD_BurstMath_CalcDeadline_Felix, dcc006479withGenSigFixup) {
	auto const height = 6479;
	auto serverSignature = HexString::arrayfrom<32>("b01f8da00aa5f6eaf801b2265c9931ecc03216ea2f62608d35df749fcfe3caad");
	uint64_t currentBaseTarget = 41866166;
	uint64_t account_nr = 1723641494112555138;

	auto currentSignature = diskcoin_generate_gensig_aes128(height, *serverSignature);

	{
		uint64_t nonce_nr = 988;
		uint32_t scoop_nr = BurstMath::calculate_scoop(height, *currentSignature); // 3608
		unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);
		EXPECT_EQ(deadline, 12696127);
	}
	{
		uint64_t nonce_nr = 44459;
		uint32_t scoop_nr = BurstMath::calculate_scoop(height, *currentSignature); // 3608
		unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);
		EXPECT_EQ(deadline, 9596714);
	}
}

TEST(RnD_BurstMath_CalcDeadline_Felix, dcc006493withGenSigFixup) {
	auto const height = 6493;
	auto serverSignature = HexString::arrayfrom<32>("c6e466a394ebfeed0f137e408a2fd61224860c12301c72c11a5c68d3d3a14f55");
	uint64_t currentBaseTarget = 30090363;
	uint64_t account_nr = 1723641494112555138;

	auto currentSignature = diskcoin_generate_gensig_aes128(height, *serverSignature);

	{
		uint64_t nonce_nr = 8540;
		uint32_t scoop_nr = BurstMath::calculate_scoop(height, *currentSignature); // 1940
		unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);
		EXPECT_EQ(deadline, 2534226);
	}
	{
		uint64_t nonce_nr = 19631;
		uint32_t scoop_nr = BurstMath::calculate_scoop(height, *currentSignature); // 1940
		unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);
		EXPECT_EQ(deadline, 1216177);
	}
}

TEST(RnD_BurstMath_CalcDeadline_Felix, dcc006548withGenSigFixup) {
	auto const height = 6548;
	auto serverSignature = HexString::arrayfrom<32>("30acd4cf4f2abb14e052b2ee79684da33ec1838d68af89171f63695e6ce43eea");
	uint64_t currentBaseTarget = 28823010;
	uint64_t account_nr = 1723641494112555138;

	auto currentSignature = diskcoin_generate_gensig_aes128(height, *serverSignature);

	{
		uint64_t nonce_nr = 54537;
		uint32_t scoop_nr = BurstMath::calculate_scoop(height, *currentSignature); // 2651
		unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);
		EXPECT_EQ(deadline, 22825302);
	}
	{
		uint64_t nonce_nr = 58049;
		uint32_t scoop_nr = BurstMath::calculate_scoop(height, *currentSignature); // 2651
		unsigned long long deadline = BurstMath::calcdeadline(account_nr, nonce_nr, scoop_nr, currentBaseTarget, *currentSignature);
		EXPECT_EQ(deadline, 17892974);
	}
}

//- diskcoin experiments, data from felixbrucker
