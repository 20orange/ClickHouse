#include <Compression/ICompressionCodec.h>
#include <Compression/CompressionInfo.h>
#include <Compression/CompressionFactory.h>
#include <zstd.h>
#include <Parsers/IAST.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTFunction.h>
#include <Common/typeid_cast.h>
#include <IO/WriteHelpers.h>
#include <IO/WriteBuffer.h>
#include <IO/BufferWithOwnMemory.h>
#include "qatseqprod.h"
#include <Poco/Logger.h>
#include <base/logger_useful.h>

namespace DB
{

class CompressionCodecZSTD : public ICompressionCodec
{
public:
    static constexpr auto ZSTD_DEFAULT_LEVEL = 1;
    static constexpr auto ZSTD_DEFAULT_LOG_WINDOW = 24;

    explicit CompressionCodecZSTD(int level_);
    CompressionCodecZSTD(int level_, int window_log);

    uint8_t getMethodByte() const override;

    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override;

    void updateHash(SipHash & hash) const override;

protected:

    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;

    void doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override;

    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return true; }

private:
    const int level;
    const bool enable_long_range;
    const int window_log;
};

namespace ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int CANNOT_DECOMPRESS;
    extern const int ILLEGAL_SYNTAX_FOR_CODEC_TYPE;
    extern const int ILLEGAL_CODEC_PARAMETER;
}

uint8_t CompressionCodecZSTD::getMethodByte() const
{
    return static_cast<uint8_t>(CompressionMethodByte::ZSTD);
}

void CompressionCodecZSTD::updateHash(SipHash & hash) const
{
    getCodecDesc()->updateTreeHash(hash);
}

UInt32 CompressionCodecZSTD::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    return ZSTD_compressBound(uncompressed_size);
}


UInt32 CompressionCodecZSTD::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
    ZSTD_CCtx * cctx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
    if (enable_long_range)
    {
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, window_log); // NB zero window_log means "use default" for libzstd
    }
    size_t compressed_size = ZSTD_compress2(cctx, dest, ZSTD_compressBound(source_size), source, source_size);
    ZSTD_freeCCtx(cctx);

    if (ZSTD_isError(compressed_size))
        throw Exception("Cannot compress block with ZSTD: " + std::string(ZSTD_getErrorName(compressed_size)), ErrorCodes::CANNOT_COMPRESS);

    return compressed_size;
}


void CompressionCodecZSTD::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
{
    size_t res = ZSTD_decompress(dest, uncompressed_size, source, source_size);

    if (ZSTD_isError(res))
        throw Exception("Cannot ZSTD_decompress: " + std::string(ZSTD_getErrorName(res)), ErrorCodes::CANNOT_DECOMPRESS);
}

CompressionCodecZSTD::CompressionCodecZSTD(int level_, int window_log_) : level(level_), enable_long_range(true), window_log(window_log_)
{
    setCodecDescription(
        "ZSTD", {std::make_shared<ASTLiteral>(static_cast<UInt64>(level)), std::make_shared<ASTLiteral>(static_cast<UInt64>(window_log))});
}

CompressionCodecZSTD::CompressionCodecZSTD(int level_) : level(level_), enable_long_range(false), window_log(0)
{
    setCodecDescription("ZSTD", {std::make_shared<ASTLiteral>(static_cast<UInt64>(level))});
}

void registerCodecZSTD(CompressionCodecFactory & factory)
{
    UInt8 method_code = UInt8(CompressionMethodByte::ZSTD);
    factory.registerCompressionCodec("ZSTD", method_code, [&](const ASTPtr & arguments) -> CompressionCodecPtr {
        int level = CompressionCodecZSTD::ZSTD_DEFAULT_LEVEL;
        if (arguments && !arguments->children.empty())
        {
            if (arguments->children.size() > 2)
                throw Exception(
                    "ZSTD codec must have 1 or 2 parameters, given " + std::to_string(arguments->children.size()),
                    ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE);

            const auto children = arguments->children;
            const auto * literal = children[0]->as<ASTLiteral>();
            if (!literal)
                throw Exception("ZSTD codec argument must be integer", ErrorCodes::ILLEGAL_CODEC_PARAMETER);

            level = literal->value.safeGet<UInt64>();
            if (level > ZSTD_maxCLevel())
                throw Exception(
                    "ZSTD codec can't have level more than " + toString(ZSTD_maxCLevel()) + ", given " + toString(level),
                    ErrorCodes::ILLEGAL_CODEC_PARAMETER);
            if (arguments->children.size() > 1)
            {
                const auto * window_literal = children[1]->as<ASTLiteral>();
                if (!window_literal)
                    throw Exception("ZSTD codec second argument must be integer", ErrorCodes::ILLEGAL_CODEC_PARAMETER);

                const int window_log = window_literal->value.safeGet<UInt64>();

                ZSTD_bounds window_log_bounds = ZSTD_cParam_getBounds(ZSTD_c_windowLog);
                if (ZSTD_isError(window_log_bounds.error))
                    throw Exception(
                        "ZSTD windowLog parameter is not supported " + std::string(ZSTD_getErrorName(window_log_bounds.error)),
                        ErrorCodes::ILLEGAL_CODEC_PARAMETER);
                // 0 means "use default" for libzstd
                if (window_log != 0 && (window_log > window_log_bounds.upperBound || window_log < window_log_bounds.lowerBound))
                    throw Exception(
                        "ZSTD codec can't have window log more than " + toString(window_log_bounds.upperBound) + " and lower than "
                            + toString(window_log_bounds.lowerBound) + ", given " + toString(window_log),
                        ErrorCodes::ILLEGAL_CODEC_PARAMETER);

                return std::make_shared<CompressionCodecZSTD>(level, window_log);
            }
        }
        return std::make_shared<CompressionCodecZSTD>(level);
    });
}

#ifdef ENABLE_QATZSTD_COMPRESSION
class CompressionCodecQATZSTD : public CompressionCodecZSTD
{
public:
    static constexpr auto QATZSTD_SUPPORTED_MIN_LEVEL = 1;
    static constexpr auto QATZSTD_SUPPORTED_MAX_LEVEL = 12;
    explicit CompressionCodecQATZSTD(int level_);
    ~CompressionCodecQATZSTD() override;

protected:
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;

private:
    const int level;
    mutable bool initialized;
    mutable ZSTD_CCtx* cctx;
    mutable void *sequenceProducerState;
    Poco::Logger * log;
};

UInt32 CompressionCodecQATZSTD::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
    if(!initialized)
    {
        cctx = ZSTD_createCCtx();
        /* Start QAT device, start QAT device at any time before compression job started */
        int res = QZSTD_startQatDevice();
        /* Create sequence producer state for QAT sequence producer */
        sequenceProducerState = QZSTD_createSeqProdState();
        /* register qatSequenceProducer */
        ZSTD_registerSequenceProducer(
            cctx,
            sequenceProducerState,
            qatSequenceProducer
        );
        /* Enable sequence producer fallback */
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableSeqProducerFallback, 1);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
        initialized = true;
        LOG_WARNING(log, "Initialization of hardware-assisted(QAT) ZSTD codec result: {} ", static_cast<UInt32>(res));
    }
    size_t compressed_size = ZSTD_compress2(cctx, dest, ZSTD_compressBound(source_size), source, source_size);

    if (ZSTD_isError(compressed_size))
        throw Exception("Cannot compress block with ZSTD: " + std::string(ZSTD_getErrorName(compressed_size)), ErrorCodes::CANNOT_COMPRESS);

    return compressed_size;
}

void registerCodecQATZSTD(CompressionCodecFactory & factory)
{
    factory.registerCompressionCodec("QATZSTD", {}, [&](const ASTPtr & arguments) -> CompressionCodecPtr
    {
        int level = CompressionCodecZSTD::ZSTD_DEFAULT_LEVEL;
        if (arguments && !arguments->children.empty())
        {
            if (arguments->children.size() > 1)
                throw Exception("QATZSTD codec must have 1 parameter, given " + std::to_string(arguments->children.size()), ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE);

            const auto children = arguments->children;
            const auto * literal = children[0]->as<ASTLiteral>();
            if (!literal)
                throw Exception("QATZSTD codec argument must be integer", ErrorCodes::ILLEGAL_CODEC_PARAMETER);

            level = literal->value.safeGet<UInt64>();
            if (level > CompressionCodecQATZSTD::QATZSTD_SUPPORTED_MAX_LEVEL || level < CompressionCodecQATZSTD::QATZSTD_SUPPORTED_MIN_LEVEL)
                throw Exception(
                    "QATZSTD codec doesn't support level more than " + toString(CompressionCodecQATZSTD::QATZSTD_SUPPORTED_MAX_LEVEL) + " and lower than "
                        + toString(CompressionCodecQATZSTD::QATZSTD_SUPPORTED_MIN_LEVEL) + ", given " + toString(level),
                    ErrorCodes::ILLEGAL_CODEC_PARAMETER);
        }

        return std::make_shared<CompressionCodecQATZSTD>(level);
    });
}

CompressionCodecQATZSTD::CompressionCodecQATZSTD(int level_)
    : CompressionCodecZSTD(level_), level(level_), initialized(false), log(&Poco::Logger::get("CompressionCodecQATZSTD"))
{
    setCodecDescription("QATZSTD", {std::make_shared<ASTLiteral>(static_cast<UInt64>(level))});
}

CompressionCodecQATZSTD::~CompressionCodecQATZSTD()
{
    if(initialized)
    {
        /* Free sequence producer state */
        QZSTD_freeSeqProdState(sequenceProducerState);
        ZSTD_freeCCtx(cctx);
    }
}
#endif

CompressionCodecPtr getCompressionCodecZSTD(int level)
{
    return std::make_shared<CompressionCodecZSTD>(level);
}

}
