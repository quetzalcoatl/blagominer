#include "BurstMath.h"

#include "../../shabal.h"

// I think I took this code from https://github.com/PoC-Consortium/Nogrod/blob/master/pkg/burstmath/libs/burstmath.c
// but really, over last few days I saw so many copies of it that I'm not sure right now
// one thing sure, I had to adapt it a little since some macros and constants were not present in this project
// and I also added comments separating generating-the-plot from the actual deadline-calculation
namespace BurstMath
{
	uint32_t calculate_scoop(uint64_t height, uint8_t *gensig) {
		sph_shabal_context sc;
		uint8_t new_gensig[32];

		sph_shabal256_init(&sc);
		sph_shabal256(&sc, gensig, 32);

		uint8_t height_swapped[8];
		//* SET_NONCE made it in one go, I didn't want to copy a macro, so for now: copy&swap
		memcpy_s(height_swapped, sizeof(height_swapped), &height, 8);
		//*byte swap time!
		std::swap(*(height_swapped + 0), *(height_swapped + 7));
		std::swap(*(height_swapped + 1), *(height_swapped + 6));
		std::swap(*(height_swapped + 2), *(height_swapped + 5));
		std::swap(*(height_swapped + 3), *(height_swapped + 4));
		//*!emit paws etyb

		sph_shabal256(&sc, (unsigned char *)&height_swapped, sizeof(height_swapped));
		sph_shabal256_close(&sc, new_gensig);

		return ((new_gensig[30] & 0x0F) << 8) | new_gensig[31];
	}

	uint64_t calcdeadline(uint64_t account_nr, uint64_t nonce_nr, uint32_t scoop_nr, uint64_t currentBaseTarget, uint8_t currentSignature[32]) {
		///* simulate reading ACCOUNT:NONCENR:SCOOPNR from files == generate the part of the plot..
		const auto NONCE_SIZE = 4096 * 64;
		const auto HASH_SIZE = 32;
		const auto HASH_CAP = 4096;
		const auto SCOOP_SIZE = 64;

		char final[32];
		char gendata[16 + NONCE_SIZE];

		//* SET_NONCE made it in one go, I didn't want to copy a macro, so for now: copy&swap
		memcpy_s(gendata + NONCE_SIZE + 0, sizeof(gendata), &account_nr, 8);
		//*byte swap time!
		std::swap(*(gendata + NONCE_SIZE + 0 + 0), *(gendata + NONCE_SIZE + 0 + 7));
		std::swap(*(gendata + NONCE_SIZE + 0 + 1), *(gendata + NONCE_SIZE + 0 + 6));
		std::swap(*(gendata + NONCE_SIZE + 0 + 2), *(gendata + NONCE_SIZE + 0 + 5));
		std::swap(*(gendata + NONCE_SIZE + 0 + 3), *(gendata + NONCE_SIZE + 0 + 4));
		//*!emit paws etyb

		//* SET_NONCE made it in one go, I didn't want to copy a macro, so for now: copy&swap
		memcpy_s(gendata + NONCE_SIZE + 8, sizeof(gendata) - 8, &nonce_nr, 8);
		//*byte swap time!
		std::swap(*(gendata + NONCE_SIZE + 8 + 0), *(gendata + NONCE_SIZE + 8 + 7));
		std::swap(*(gendata + NONCE_SIZE + 8 + 1), *(gendata + NONCE_SIZE + 8 + 6));
		std::swap(*(gendata + NONCE_SIZE + 8 + 2), *(gendata + NONCE_SIZE + 8 + 5));
		std::swap(*(gendata + NONCE_SIZE + 8 + 3), *(gendata + NONCE_SIZE + 8 + 4));
		//*!emit paws etyb

		sph_shabal_context x;
		int len;

		for (int i = NONCE_SIZE; i > 0; i -= HASH_SIZE) {
			sph_shabal256_init(&x);

			len = NONCE_SIZE + 16 - i;
			if (len > HASH_CAP)
				len = HASH_CAP;

			sph_shabal256(&x, (unsigned char *)&gendata[i], len);
			sph_shabal256_close(&x, &gendata[i - HASH_SIZE]);
		}

		sph_shabal256_init(&x);
		sph_shabal256(&x, (unsigned char *)gendata, 16 + NONCE_SIZE);
		sph_shabal256_close(&x, final);

		// XOR with final
		for (int i = 0; i < NONCE_SIZE; i++)
			gendata[i] ^= (final[i % 32]);

		uint8_t scoop[SCOOP_SIZE];
		memcpy(scoop, gendata + (scoop_nr * SCOOP_SIZE), 32);
		memcpy(scoop + 32, gendata + ((4095 - scoop_nr) * SCOOP_SIZE) + 32, 32);
		///* now we have the data 'read from the plot files' in SCOOP array

		///* back to orig algo from blago shabal.cpp:procscoop_sph()
		sph_shabal_context xx;
		sph_shabal256_init(&xx); // = init(global_32)
		char sig[32 + 64 + 64];
		char res[32];
		///*
		memcpy_s(sig, sizeof(sig), currentSignature, sizeof(char) * 32);
		memcpy_s(&sig[32], sizeof(sig) - 32, scoop, sizeof(char) * 64);

		sph_shabal256(&xx, (const unsigned char*)sig, 64 + 32);
		sph_shabal256_close(&xx, res);

		unsigned long long *wertung = (unsigned long long*)res;

		unsigned long long deadline = *wertung / currentBaseTarget;

		return deadline;
	}
}