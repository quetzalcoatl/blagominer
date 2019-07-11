#pragma once

#include <string>
#include <vector>

class HexString
{
private:
	static uint8_t char2int(char input)
	{
		if (input >= '0' && input <= '9') return input - '0';
		if (input >= 'A' && input <= 'F') return input - 'A' + 10;
		if (input >= 'a' && input <= 'f') return input - 'a' + 10;
		throw std::invalid_argument("Invalid input string");
	}

	static void hex2bin(std::string const& src, uint8_t* target)
	{
		char const * source = src.c_str();

		if (src.length() % 2 == 1)
			*(target++) = char2int(*(source++));

		while (source[0] && source[1])
		{
			*(target++) = char2int(source[0]) * 16 + char2int(source[1]);
			source += 2;
		}
	}

public:

	/**
	 * Parses a string of hexadecimal digits into an array of bytes.
	 * Assumes natural order of bits in byte: 0x01 is 1, 0x10 is 16.
	 * Assumes natural order of bytes in string: AABB is [0xAA,0xBB].
	 *
	 * @param text the text to parse
	 * @return vector of 8bit bytes
	 * @throws std::invalid_argument when input contains non-hexdigit characters
	 */
	static std::vector<uint8_t> from(std::string const& text)
	{
		std::vector<uint8_t> tmp;
		tmp.resize((text.size() + 1) / 2);
		hex2bin(text, tmp.data());
		return tmp;
	}
};