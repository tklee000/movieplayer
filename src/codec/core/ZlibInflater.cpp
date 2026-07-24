#include "codec/core/ZlibInflater.h"

#include <array>
#include <limits>

namespace movieplayer::codec {
namespace {

constexpr std::size_t kMaximumOutputBytes = 64U * 1024U * 1024U;
constexpr unsigned kMaximumCodeBits = 15;

class DeflateBits {
public:
    DeflateBits(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    bool Read(unsigned count, unsigned& value) {
        if (count > 24) return false;
        while (available_ < count) {
            if (position_ >= size_) return false;
            bits_ |= static_cast<std::uint64_t>(data_[position_++]) << available_;
            available_ += 8;
        }
        const std::uint64_t mask = count == 0 ? 0 :
            ((std::uint64_t{1} << count) - 1U);
        value = static_cast<unsigned>(bits_ & mask);
        bits_ >>= count;
        available_ -= count;
        return true;
    }

    void AlignByte() {
        const unsigned discard = available_ & 7U;
        bits_ >>= discard;
        available_ -= discard;
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t position_ = 0;
    std::uint64_t bits_ = 0;
    unsigned available_ = 0;
};

struct Huffman {
    std::array<unsigned, kMaximumCodeBits + 1> counts{};
    std::vector<unsigned> symbols;
};

bool BuildHuffman(const std::vector<unsigned>& lengths, Huffman& tree) {
    tree = {};
    tree.symbols.resize(lengths.size());
    for (unsigned length : lengths) {
        if (length > kMaximumCodeBits) return false;
        ++tree.counts[length];
    }
    if (tree.counts[0] == lengths.size()) return true;

    int codesLeft = 1;
    for (unsigned length = 1; length <= kMaximumCodeBits; ++length) {
        codesLeft = (codesLeft << 1) - static_cast<int>(tree.counts[length]);
        if (codesLeft < 0) return false;
    }

    std::array<unsigned, kMaximumCodeBits + 1> offsets{};
    offsets[1] = 0;
    for (unsigned length = 1; length < kMaximumCodeBits; ++length) {
        offsets[length + 1] = offsets[length] + tree.counts[length];
    }
    for (unsigned symbol = 0; symbol < lengths.size(); ++symbol) {
        const unsigned length = lengths[symbol];
        if (length != 0) tree.symbols[offsets[length]++] = symbol;
    }
    return true;
}

bool DecodeSymbol(DeflateBits& bits, const Huffman& tree, unsigned& symbol) {
    unsigned code = 0;
    unsigned first = 0;
    unsigned index = 0;
    for (unsigned length = 1; length <= kMaximumCodeBits; ++length) {
        unsigned bit = 0;
        if (!bits.Read(1, bit)) return false;
        code |= bit;
        const unsigned count = tree.counts[length];
        if (code >= first && code - first < count) {
            const unsigned position = index + code - first;
            if (position >= tree.symbols.size()) return false;
            symbol = tree.symbols[position];
            return true;
        }
        index += count;
        first = (first + count) << 1U;
        code <<= 1U;
    }
    return false;
}

bool AppendByte(std::vector<std::uint8_t>& output, std::uint8_t value) {
    if (output.size() >= kMaximumOutputBytes) return false;
    output.push_back(value);
    return true;
}

bool DecodeCompressedBlock(DeflateBits& bits, const Huffman& literals,
                           const Huffman& distances,
                           std::vector<std::uint8_t>& output) {
    static constexpr unsigned lengthBase[] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
    };
    static constexpr unsigned lengthExtra[] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
    };
    static constexpr unsigned distanceBase[] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
        6145, 8193, 12289, 16385, 24577,
    };
    static constexpr unsigned distanceExtra[] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
    };

    for (;;) {
        unsigned symbol = 0;
        if (!DecodeSymbol(bits, literals, symbol)) return false;
        if (symbol < 256) {
            if (!AppendByte(output, static_cast<std::uint8_t>(symbol))) return false;
            continue;
        }
        if (symbol == 256) return true;
        if (symbol < 257 || symbol > 285) return false;

        const unsigned lengthIndex = symbol - 257;
        unsigned extra = 0;
        if (!bits.Read(lengthExtra[lengthIndex], extra)) return false;
        const unsigned length = lengthBase[lengthIndex] + extra;

        unsigned distanceSymbol = 0;
        if (!DecodeSymbol(bits, distances, distanceSymbol) ||
            distanceSymbol >= 30) {
            return false;
        }
        if (!bits.Read(distanceExtra[distanceSymbol], extra)) return false;
        const unsigned distance = distanceBase[distanceSymbol] + extra;
        if (distance == 0 || distance > output.size() ||
            length > kMaximumOutputBytes - output.size()) {
            return false;
        }
        for (unsigned i = 0; i < length; ++i) {
            output.push_back(output[output.size() - distance]);
        }
    }
}

bool BuildFixedTrees(Huffman& literals, Huffman& distances) {
    std::vector<unsigned> literalLengths(288, 8);
    for (unsigned i = 144; i <= 255; ++i) literalLengths[i] = 9;
    for (unsigned i = 256; i <= 279; ++i) literalLengths[i] = 7;
    std::vector<unsigned> distanceLengths(32, 5);
    return BuildHuffman(literalLengths, literals) &&
           BuildHuffman(distanceLengths, distances);
}

bool BuildDynamicTrees(DeflateBits& bits, Huffman& literals,
                       Huffman& distances) {
    unsigned hlit = 0, hdist = 0, hclen = 0;
    if (!bits.Read(5, hlit) || !bits.Read(5, hdist) ||
        !bits.Read(4, hclen)) {
        return false;
    }
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 32) return false;

    static constexpr unsigned order[] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
    };
    std::vector<unsigned> codeLengths(19, 0);
    for (unsigned i = 0; i < hclen; ++i) {
        if (!bits.Read(3, codeLengths[order[i]])) return false;
    }
    Huffman codeTree;
    if (!BuildHuffman(codeLengths, codeTree)) return false;

    std::vector<unsigned> lengths;
    lengths.reserve(hlit + hdist);
    while (lengths.size() < hlit + hdist) {
        unsigned symbol = 0;
        if (!DecodeSymbol(bits, codeTree, symbol)) return false;
        if (symbol <= 15) {
            lengths.push_back(symbol);
            continue;
        }
        unsigned repeatBits = 0;
        unsigned repeatBase = 0;
        unsigned repeatedValue = 0;
        if (symbol == 16) {
            if (lengths.empty()) return false;
            repeatBits = 2;
            repeatBase = 3;
            repeatedValue = lengths.back();
        } else if (symbol == 17) {
            repeatBits = 3;
            repeatBase = 3;
        } else if (symbol == 18) {
            repeatBits = 7;
            repeatBase = 11;
        } else {
            return false;
        }
        unsigned extra = 0;
        if (!bits.Read(repeatBits, extra)) return false;
        const unsigned repeat = repeatBase + extra;
        if (repeat > hlit + hdist - lengths.size()) return false;
        lengths.insert(lengths.end(), repeat, repeatedValue);
    }
    std::vector<unsigned> literalLengths(lengths.begin(),
                                         lengths.begin() + hlit);
    std::vector<unsigned> distanceLengths(lengths.begin() + hlit,
                                          lengths.end());
    return literalLengths.size() > 256 && literalLengths[256] != 0 &&
           BuildHuffman(literalLengths, literals) &&
           BuildHuffman(distanceLengths, distances);
}

std::uint32_t Adler32(const std::vector<std::uint8_t>& data) {
    constexpr std::uint32_t modulus = 65521;
    std::uint32_t first = 1;
    std::uint32_t second = 0;
    for (std::uint8_t value : data) {
        first = (first + value) % modulus;
        second = (second + first) % modulus;
    }
    return (second << 16U) | first;
}

}  // namespace

bool InflateZlib(const std::uint8_t* data, std::size_t size,
                 std::vector<std::uint8_t>& output, std::wstring& error) {
    output.clear();
    if (!data || size < 6) {
        error = L"The zlib stream is truncated";
        return false;
    }
    const unsigned cmf = data[0];
    const unsigned flags = data[1];
    if ((cmf & 0x0fU) != 8 || (cmf >> 4U) > 7 ||
        ((cmf << 8U) + flags) % 31U != 0 || (flags & 0x20U) != 0) {
        error = L"The zlib stream has an unsupported header";
        return false;
    }

    DeflateBits bits(data + 2, size - 6);
    bool finalBlock = false;
    while (!finalBlock) {
        unsigned final = 0, type = 0;
        if (!bits.Read(1, final) || !bits.Read(2, type)) {
            error = L"The DEFLATE block header is truncated";
            return false;
        }
        finalBlock = final != 0;
        if (type == 0) {
            bits.AlignByte();
            unsigned length = 0, inverse = 0;
            if (!bits.Read(16, length) || !bits.Read(16, inverse) ||
                (length ^ 0xffffU) != inverse ||
                length > kMaximumOutputBytes - output.size()) {
                error = L"The uncompressed DEFLATE block is invalid";
                return false;
            }
            for (unsigned i = 0; i < length; ++i) {
                unsigned value = 0;
                if (!bits.Read(8, value)) {
                    error = L"The uncompressed DEFLATE block is truncated";
                    return false;
                }
                output.push_back(static_cast<std::uint8_t>(value));
            }
        } else if (type == 1 || type == 2) {
            Huffman literals, distances;
            const bool built = type == 1 ? BuildFixedTrees(literals, distances)
                                         : BuildDynamicTrees(bits, literals, distances);
            if (!built || !DecodeCompressedBlock(bits, literals, distances,
                                                  output)) {
                error = L"The compressed DEFLATE block is invalid";
                return false;
            }
        } else {
            error = L"The DEFLATE block uses a reserved type";
            return false;
        }
    }

    const std::uint32_t expected =
        (static_cast<std::uint32_t>(data[size - 4]) << 24U) |
        (static_cast<std::uint32_t>(data[size - 3]) << 16U) |
        (static_cast<std::uint32_t>(data[size - 2]) << 8U) |
        data[size - 1];
    if (Adler32(output) != expected) {
        output.clear();
        error = L"The zlib Adler-32 checksum does not match";
        return false;
    }
    error.clear();
    return true;
}

}  // namespace movieplayer::codec
