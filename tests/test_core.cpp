// Identities, exact units, canonical encoding and digests.
#include "cooling_failover/canonical.hpp"
#include "test_framework.hpp"

#include <limits>
#include <string>
#include <vector>

using namespace cooling_failover;

namespace {

std::vector<std::uint8_t> bytes_of(const std::string& text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::string hex_of(std::string_view text) {
  return to_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                              text.size()));
}

}  // namespace

CF_TEST(Ids, RoundTripAndCanonicalRendering) {
  const FailoverDomainId domain = FailoverDomainId::from_value(0x0123456789abcdefULL);
  CF_CHECK_EQ(domain.to_string(), std::string("0x0123456789abcdef"));
  CF_CHECK_EQ(domain.value(), 0x0123456789abcdefULL);
  CF_CHECK_OK(FailoverDomainId::parse("0x0123456789abcdef", "domain"));
  CF_CHECK_EQ(*FailoverDomainId::parse("0x0123456789abcdef", "domain"), domain);
  CF_CHECK_EQ(*FailoverDomainId::parse("81985529216486895", "domain"), domain);
  CF_CHECK(FailoverDomainId::from_value(0).is_nil());
  CF_CHECK(!domain.is_nil());
}

CF_TEST(Ids, MalformedInputIsRejectedDeterministically) {
  CF_CHECK_ERROR(FailoverDomainId::parse("", "domain"), ErrorCode::EmptyRequiredField);
  CF_CHECK_ERROR(FailoverDomainId::parse("0x", "domain"), ErrorCode::MalformedIdentity);
  CF_CHECK_ERROR(FailoverDomainId::parse("0xzz", "domain"), ErrorCode::MalformedIdentity);
  CF_CHECK_ERROR(FailoverDomainId::parse(" 12", "domain"), ErrorCode::MalformedIdentity);
  CF_CHECK_ERROR(FailoverDomainId::parse("012", "domain"), ErrorCode::MalformedIdentity);
  CF_CHECK_ERROR(FailoverDomainId::parse("99999999999999999999999", "domain"),
                 ErrorCode::MalformedIdentity);
  // A literal that does not fit in 64 bits is out of range, not malformed.
  CF_CHECK_ERROR(FailoverDomainId::parse("0xfffffffffffffffff", "domain"), ErrorCode::OutOfRange);
  CF_CHECK_ERROR(FailoverDomainId::parse("18446744073709551616", "domain"), ErrorCode::OutOfRange);
  CF_CHECK_ERROR(FailoverDomainId::parse("\xff\xfe", "domain"), ErrorCode::MalformedIdentity);
  CF_CHECK_EQ((*FailoverDomainId::parse("18446744073709551615", "domain")).value(),
              std::numeric_limits<std::uint64_t>::max());
}

CF_TEST(Ids, GenerationsAndCountersSaturateSafely) {
  const TopologyGeneration maximum =
      TopologyGeneration::from_value(std::numeric_limits<std::uint64_t>::max());
  CF_CHECK_ERROR(maximum.next(), ErrorCode::Overflow);
  CF_CHECK_EQ(*TopologyGeneration::from_value(4).next(), TopologyGeneration::from_value(5));
  CF_CHECK(!TopologyGeneration::unset().is_set());

  const StateRevision maximum_revision =
      StateRevision::from_value(std::numeric_limits<std::uint64_t>::max());
  CF_CHECK_ERROR(maximum_revision.next(), ErrorCode::Overflow);
  CF_CHECK_ERROR(StateRevision::from_value(10).advanced_by(UINT64_MAX), ErrorCode::Overflow);

  const Tick early = Tick::from_value(10);
  const Tick late = Tick::from_value(25);
  CF_CHECK_EQ(late.elapsed_since(early), 15U);
  CF_CHECK_EQ(early.elapsed_since(late), 0U);  // saturating, never negative
  CF_CHECK_ERROR(Tick::from_value(UINT64_MAX).advanced_by(1), ErrorCode::Overflow);
}

CF_TEST(Units, ExactIntegerArithmeticAndOverflow) {
  const Watts maximum = Watts::from_watts(Watts::max_watts());
  CF_CHECK_ERROR(Watts::add(maximum, Watts::from_watts(1)), ErrorCode::Overflow);
  CF_CHECK_ERROR(Watts::subtract(Watts::from_watts(Watts::min_watts()), Watts::from_watts(1)),
                 ErrorCode::Overflow);
  CF_CHECK_EQ(*Watts::add(Watts::from_watts(10), Watts::from_watts(32)), Watts::from_watts(42));

  const std::int64_t values[] = {1, 2, 3, 4};
  CF_CHECK_EQ(*Watts::sum(values, 4), Watts::from_watts(10));
  const std::int64_t overflow_values[] = {Watts::max_watts(), 1};
  CF_CHECK_ERROR(Watts::sum(overflow_values, 2), ErrorCode::Overflow);

  const BasisPoints ten_percent = *BasisPoints::from_value(1000);
  CF_CHECK_EQ(*Watts::scale_up(Watts::from_watts(1000), ten_percent), Watts::from_watts(100));
  // Rounding is away from zero, so a non-zero margin never becomes zero.
  CF_CHECK_EQ(*Watts::scale_up(Watts::from_watts(1), *BasisPoints::from_value(1)),
              Watts::from_watts(1));
  CF_CHECK_EQ(*Watts::scale_up(Watts::zero(), ten_percent), Watts::zero());
  CF_CHECK_EQ(*Watts::scale_up(Watts::from_watts(-1000), ten_percent), Watts::from_watts(-100));
  CF_CHECK_EQ(*Watts::scale_up(maximum, *BasisPoints::from_value(10000)), maximum);
  CF_CHECK_ERROR(BasisPoints::from_value(10001), ErrorCode::OutOfRange);
  CF_CHECK_ERROR(BasisPoints::from_value(-1), ErrorCode::OutOfRange);
  CF_CHECK_EQ(Watts::from_watts(1500).to_string(), std::string("1500W"));
  CF_CHECK_EQ(Watts::from_watts(-1500).to_string(), std::string("-1500W"));
}

CF_TEST(Digests, Sha256KnownAnswers) {
  CF_CHECK_EQ(sha256(bytes_of("")).to_hex(),
              std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CF_CHECK_EQ(sha256(bytes_of("abc")).to_hex(),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CF_CHECK_EQ(
      sha256(bytes_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")).to_hex(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  CF_CHECK_EQ(
      sha256(bytes_of("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                      "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"))
          .to_hex(),
      std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
  // A message spanning several compression blocks.
  std::string long_message(1000, 'a');
  Digest incremental;
  {
    Sha256 hasher;
    hasher.update(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(long_message.data()), long_message.size()));
    incremental = hasher.finish();
  }
  CF_CHECK_EQ(incremental.to_hex(), sha256(bytes_of(long_message)).to_hex());
  CF_CHECK_OK(Digest::from_hex(incremental.to_hex()));
  CF_CHECK_EQ(*Digest::from_hex(incremental.to_hex()), incremental);
  CF_CHECK_ERROR(Digest::from_hex("00"), ErrorCode::MalformedIdentity);
  CF_CHECK(sha256({}).is_zero() == false);
}

CF_TEST(Integrity, Crc32KnownAnswer) {
  CF_CHECK_EQ(crc32(bytes_of("123456789")), 0xCBF43926U);
  CF_CHECK_EQ(crc32(bytes_of("")), 0U);
  const std::vector<std::uint8_t> payload = bytes_of("cooling-failover");
  const std::uint32_t split_a = crc32(payload);
  const std::uint32_t split_b =
      crc32_extend(crc32_extend(0U, std::span<const std::uint8_t>(payload.data(), 4)),
                   std::span<const std::uint8_t>(payload.data() + 4, payload.size() - 4));
  CF_CHECK_EQ(split_a, split_b);
}

CF_TEST(Canonical, EncoderDecoderRoundTripAndStrictness) {
  Encoder encoder;
  encoder.u8(7);
  encoder.u32(0xDEADBEEF);
  encoder.u64(0x0123456789abcdefULL);
  encoder.i64(-42);
  encoder.boolean(true);
  encoder.text("hello");
  encoder.id(FailoverDomainId::from_value(9));
  encoder.generation(TopologyGeneration::from_value(3));
  encoder.count(2);
  encoder.u8(0);
  encoder.u8(0);
  const std::vector<std::uint8_t> bytes = encoder.data();
  CF_CHECK(encoder.ok());

  Decoder decoder(bytes);
  CF_CHECK_EQ(static_cast<int>(decoder.u8()), 7);
  CF_CHECK_EQ(decoder.u32(), 0xDEADBEEFu);
  CF_CHECK_EQ(decoder.u64(), 0x0123456789abcdefULL);
  CF_CHECK_EQ(decoder.i64(), -42);
  CF_CHECK_EQ(decoder.boolean(), true);
  CF_CHECK_EQ(decoder.text(), std::string("hello"));
  CF_CHECK_EQ(decoder.id<FailoverDomainTag>(), FailoverDomainId::from_value(9));
  CF_CHECK_EQ(decoder.generation<TopologyGenerationTag>(), TopologyGeneration::from_value(3));
  CF_CHECK_EQ(decoder.count(), std::size_t{2});
  decoder.u8();
  decoder.u8();
  CF_CHECK_OK(decoder.require_end("round trip"));
  CF_CHECK(decoder.ok());

  // Truncation at every length must be reported when the same field sequence is
  // decoded, never silently accepted.
  const auto decode_sequence = [](std::span<const std::uint8_t> input) {
    Decoder reader(input);
    reader.u8();
    reader.u32();
    reader.u64();
    reader.i64();
    reader.boolean();
    reader.text();
    (void)reader.id<FailoverDomainTag>();
    (void)reader.generation<TopologyGenerationTag>();
    const std::size_t items = reader.count();
    for (std::size_t index = 0; index < items; ++index) {
      reader.u8();
    }
    return reader.status();
  };
  CF_CHECK(decode_sequence(bytes).ok());
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    const Status outcome = decode_sequence(std::span<const std::uint8_t>(bytes.data(), length));
    CF_CHECK_MSG(!outcome.ok(), "truncation to " + std::to_string(length) + " was accepted");
    CF_CHECK_EQ(outcome.code(), ErrorCode::StoreTruncated);
  }

  // Trailing bytes are rejected once the field sequence has been decoded.
  std::vector<std::uint8_t> extended = bytes;
  extended.push_back(0xAB);
  Decoder trailing(extended);
  trailing.u8();
  trailing.u32();
  trailing.u64();
  trailing.i64();
  trailing.boolean();
  (void)trailing.text();
  (void)trailing.id<FailoverDomainTag>();
  (void)trailing.generation<TopologyGenerationTag>();
  const std::size_t trailing_items = trailing.count();
  for (std::size_t index = 0; index < trailing_items; ++index) {
    trailing.u8();
  }
  CF_CHECK_ERROR(trailing.require_end("trailing"), ErrorCode::StoreTrailingBytes);
  CF_CHECK(trailing.ok());
}

CF_TEST(Canonical, DecoderRejectsImpossibleCountsAndReservedFields) {
  Encoder encoder;
  encoder.u32(0xFFFFFFFFU);
  Decoder huge(encoder.data());
  CF_CHECK_EQ(huge.count(), std::size_t{0});
  CF_CHECK(!huge.ok());
  CF_CHECK_EQ(huge.status().code(), ErrorCode::BoundsExceeded);

  Encoder reserved;
  reserved.u32(7);
  Decoder reserved_decoder(reserved.data());
  CF_CHECK(!reserved_decoder.reserved_zero("reserved"));
  CF_CHECK_EQ(reserved_decoder.status().code(), ErrorCode::StoreReservedFieldSet);

  Encoder boolean_encoder;
  boolean_encoder.u8(2);
  Decoder boolean_decoder(boolean_encoder.data());
  CF_CHECK_EQ(boolean_decoder.boolean(), false);
  CF_CHECK_EQ(boolean_decoder.status().code(), ErrorCode::InvalidEnumValue);

  Encoder basis;
  basis.u32(10001);
  Decoder basis_decoder(basis.data());
  CF_CHECK_ERROR(basis_decoder.basis_points(), ErrorCode::OutOfRange);

  Encoder big_text;
  big_text.u32(static_cast<std::uint32_t>(kMaxTextFieldBytes + 1));
  Decoder big_text_decoder(big_text.data());
  CF_CHECK_EQ(big_text_decoder.text(), std::string());
  CF_CHECK_EQ(big_text_decoder.status().code(), ErrorCode::BoundsExceeded);

  Encoder oversized;
  oversized.count(kMaxCollectionItems + 1);
  CF_CHECK(!oversized.ok());
  CF_CHECK_EQ(oversized.status().code(), ErrorCode::BoundsExceeded);
}

CF_TEST(Canonical, ByteForByteDeterminismOfEqualValues) {
  auto build = [](int variant) {
    Encoder encoder;
    encoder.text("cooling");
    encoder.id(SourceGroupId::from_value(11));
    encoder.watts(Watts::from_watts(variant));
    return encoder.data();
  };
  CF_CHECK(build(5) == build(5));
  CF_CHECK(build(5) != build(6));
}

CF_TEST(Status, ContextIsOrderedAndComparable) {
  const Status first = Status::error(ErrorCode::OutOfRange, "boom", "beta", 2, "alpha", 1);
  const Status second = Status::error(ErrorCode::OutOfRange, "boom", "alpha", 1, "beta", 2);
  CF_CHECK_EQ(first, second);
  CF_CHECK_EQ(first.canonical_line(), second.canonical_line());
  CF_CHECK_EQ(std::string(to_string(ErrorCode::StoreLocked)), std::string("StoreLocked"));
  CF_CHECK(classify(ErrorCode::StaleGeneration) == ErrorClass::Stale);
  CF_CHECK(classify(ErrorCode::AuthorityDenied) == ErrorClass::Denied);
  CF_CHECK(classify(ErrorCode::StoreVersionUnsupported) == ErrorClass::Unsupported);
  CF_CHECK(Status::success().ok());
  CF_CHECK_EQ(Status::success().canonical_line(), std::string("Ok(0)"));
}

CF_TEST(Hex, RoundTripAndRejection) {
  const std::vector<std::uint8_t> data = {0x00, 0x0F, 0xF0, 0xFF};
  const std::string text = to_hex(data);
  CF_CHECK_EQ(text, std::string("000ff0ff"));
  CF_CHECK_EQ(*from_hex(text), data);
  CF_CHECK_ERROR(from_hex("abc"), ErrorCode::InvalidText);
  CF_CHECK_ERROR(from_hex("zz"), ErrorCode::InvalidText);
}
