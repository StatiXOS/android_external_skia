/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/core/SkPaintParamsKey.h"

#include <cstring>
#include "src/core/SkKeyHelpers.h"
#include "src/core/SkShaderCodeDictionary.h"

//--------------------------------------------------------------------------------------------------
SkPaintParamsKeyBuilder::SkPaintParamsKeyBuilder(const SkShaderCodeDictionary* dict,
                                                 SkBackend backend)
        : fDict(dict)
        , fBackend(backend) {
}

#ifdef SK_DEBUG
void SkPaintParamsKeyBuilder::checkReset() {
    SkASSERT(!this->isLocked());
    SkASSERT(this->sizeInBytes() == 0);
    SkASSERT(fIsValid);
    SkASSERT(fStack.empty());
}
#endif

// Block headers have the following structure:
//  1st byte: codeSnippetID
//  2nd byte: total blockSize in bytes
// This call stores the header's offset in the key on the stack to be used in 'endBlock'
void SkPaintParamsKeyBuilder::beginBlock(int codeSnippetID) {
    if (!this->isValid()) {
        return;
    }

    if (codeSnippetID < 0 || codeSnippetID > fDict->maxCodeSnippetID()) {
        // SKGPU_LOG_W("Unknown code snippet ID.");
        this->makeInvalid();
        return;
    }

#ifdef SK_DEBUG
    if (!fStack.empty()) {
        // The children of a block should appear before any of the parent's data
        SkASSERT(fStack.back().fCurDataPayloadEntry == 0);
    }

    static const SkPaintParamsKey::DataPayloadField kHeader[2] = {
            {"snippetID", SkPaintParamsKey::DataPayloadType::kByte, 1},
            {"blockSize", SkPaintParamsKey::DataPayloadType::kByte, 1},
    };

    static const SkSpan<const SkPaintParamsKey::DataPayloadField> kHeaderExpectations(kHeader, 2);
#endif

    SkASSERT(!this->isLocked());

    fData.reserve_back(SkPaintParamsKey::kBlockHeaderSizeInBytes);
    fStack.push_back({ codeSnippetID, this->sizeInBytes(),
                       SkDEBUGCODE(kHeaderExpectations, 0) });

    this->addByte(SkTo<uint8_t>(codeSnippetID));
    this->addByte(0);  // this needs to be patched up with a call to endBlock

#ifdef SK_DEBUG
    fStack.back().fDataPayloadExpectations = fDict->dataPayloadExpectations(codeSnippetID);
    fStack.back().fCurDataPayloadEntry = 0;
#endif
}

// Update the size byte of a block header
void SkPaintParamsKeyBuilder::endBlock() {
    if (!this->isValid()) {
        return;
    }

    if (fStack.empty()) {
        // SKGPU_LOG_W("Mismatched beginBlock/endBlocks.");
        this->makeInvalid();
        return;
    }

    // All the expected fields should be filled in at this point
    SkASSERT(fStack.back().fCurDataPayloadEntry ==
             SkTo<int>(fStack.back().fDataPayloadExpectations.size()));
    SkASSERT(!this->isLocked());

    int headerOffset = fStack.back().fHeaderOffset;

    SkASSERT(fData[headerOffset] == fStack.back().fCodeSnippetID);
    SkASSERT(fData[headerOffset+SkPaintParamsKey::kBlockSizeOffsetInBytes] == 0);

    int blockSize = this->sizeInBytes() - headerOffset;
    if (blockSize > SkPaintParamsKey::kMaxBlockSize) {
        // SKGPU_LOG_W("Key's data payload is too large.");
        this->makeInvalid();
        return;
    }

    fData[headerOffset+SkPaintParamsKey::kBlockSizeOffsetInBytes] = blockSize;

    fStack.pop_back();

#ifdef SK_DEBUG
    if (!fStack.empty()) {
        // The children of a block should appear before any of the parent's data
        SkASSERT(fStack.back().fCurDataPayloadEntry == 0);
    }
#endif
}

void SkPaintParamsKeyBuilder::addBytes(uint32_t numBytes, const uint8_t* data) {
    if (!this->isValid()) {
        return;
    }

    if (fStack.empty()) {
        // SKGPU_LOG_W("Missing call to 'beginBlock'.");
        this->makeInvalid();
        return;
    }

#ifdef SK_DEBUG
    const StackFrame& frame = fStack.back();
    const auto& expectations = frame.fDataPayloadExpectations;

    // TODO: right now we reject writing 'n' bytes one at a time. We could allow it by tracking
    // the number of bytes written in the stack frame.
    SkASSERT(frame.fCurDataPayloadEntry < SkTo<int>(expectations.size()) &&
             expectations.data() &&
             expectations[frame.fCurDataPayloadEntry].fType ==
                                                        SkPaintParamsKey::DataPayloadType::kByte &&
             expectations[frame.fCurDataPayloadEntry].fCount == numBytes);

    fStack.back().fCurDataPayloadEntry++;
#endif

    SkASSERT(!this->isLocked());

    fData.push_back_n(numBytes, data);
}

SkPaintParamsKey SkPaintParamsKeyBuilder::lockAsKey() {
    if (!fStack.empty()) {
        // SKGPU_LOG_W("Mismatched beginBlock/endBlocks.");
        this->makeInvalid();  // fall through
    }

    SkASSERT(!this->isLocked());

    // Partially reset for reuse. Note that the key resulting from this call will be holding a lock
    // on this builder and must be deleted before this builder is fully reset.
    fIsValid = true;
    fStack.clear();

    return SkPaintParamsKey(SkMakeSpan(fData.data(), fData.count()), this);
}

void SkPaintParamsKeyBuilder::makeInvalid() {
    SkASSERT(fIsValid);
    SkASSERT(!this->isLocked());

    fStack.clear();
    fData.reset();
    this->beginBlock(SkBuiltInCodeSnippetID::kError);
    this->endBlock();

    SkASSERT(fIsValid);
    fIsValid = false;
}

//--------------------------------------------------------------------------------------------------
SkPaintParamsKey::SkPaintParamsKey(SkSpan<const uint8_t> span,
                                   SkPaintParamsKeyBuilder* originatingBuilder)
        : fData(span)
        , fOriginatingBuilder(originatingBuilder) {
    fOriginatingBuilder->lock();
}

SkPaintParamsKey::SkPaintParamsKey(SkSpan<const uint8_t> rawData)
        : fData(rawData)
        , fOriginatingBuilder(nullptr) {
}

SkPaintParamsKey::~SkPaintParamsKey() {
    if (fOriginatingBuilder) {
        fOriginatingBuilder->unlock();
    }
}

bool SkPaintParamsKey::operator==(const SkPaintParamsKey& that) const {
    return fData.size() == that.fData.size() &&
           !memcmp(fData.data(), that.fData.data(), fData.size());
}

#ifdef SK_DEBUG

namespace {

void output_indent(int indent) {
    for (int i = 0; i < indent; ++i) {
        SkDebugf("    ");
    }
}

int dump_block(const SkShaderCodeDictionary* dict,
               const SkPaintParamsKey& key,
               int headerOffset,
               int indent) {
    uint8_t id = key.byte(headerOffset);
    uint8_t blockSize = key.byte(headerOffset+1);
    SkASSERT(blockSize >= 2 && headerOffset+blockSize <= key.sizeInBytes());

    auto entry = dict->getEntry(id);
    if (!entry) {
        output_indent(indent);
        SkDebugf("unknown block! (%dB)\n", blockSize);
    }

    output_indent(indent);
    SkDebugf("%s block (%dB)\n", entry->fStaticFunctionName, blockSize);

    int curOffset = headerOffset + SkPaintParamsKey::kBlockHeaderSizeInBytes;

    for (int i = 0; i < entry->fNumChildren; ++i) {
        output_indent(indent);
        // TODO: it would be nice if the names of the children were also stored (i.e., "src"/"dst")
        SkDebugf("child %d:\n", i);

        int childSize = dump_block(dict, key, curOffset, indent+1);
        curOffset += childSize;
    }

    for (auto e : entry->fDataPayloadExpectations) {
        output_indent(indent);
        SkDebugf("%s[%d]: ", e.fName, e.fCount);
        // TODO: add some sort of templatized reader object for this
        SkASSERT(e.fType == SkPaintParamsKey::DataPayloadType::kByte);
        for (uint32_t i = 0; i < e.fCount; ++i) {
            output_indent(indent);
            SkDebugf("%d,", key.byte(curOffset));
            ++curOffset;
        }
        SkDebugf("\n");
    }

    return blockSize;
}

} // anonymous namespace

// This just iterates over the top-level blocks calling block-specific dump methods.
void SkPaintParamsKey::dump(const SkShaderCodeDictionary* dict) const {
    SkDebugf("--------------------------------------\n");
    SkDebugf("SkPaintParamsKey (%dB):\n", this->sizeInBytes());

    int curHeaderOffset = 0;
    while (curHeaderOffset < this->sizeInBytes()) {
        int blockSize = dump_block(dict, *this, curHeaderOffset, 0);
        curHeaderOffset += blockSize;
    }
}
#endif

int SkPaintParamsKey::AddBlockToShaderInfo(SkShaderCodeDictionary* dict,
                                           const SkPaintParamsKey& key,
                                           int headerOffset,
                                           SkShaderInfo* result) {
    auto [codeSnippetID, blockSize] = key.readCodeSnippetID(headerOffset);

    auto entry = dict->getEntry(codeSnippetID);

    result->add(*entry);

    // The child blocks appear right after the parent block's header in the key and go
    // right after the parent's SnippetEntry in the shader info
    int childOffset = headerOffset + kBlockHeaderSizeInBytes;
    for (int i = 0; i < entry->fNumChildren; ++i) {
        SkASSERT(childOffset < headerOffset + blockSize);

        int childBlockSize = AddBlockToShaderInfo(dict, key, childOffset, result);
        childOffset += childBlockSize;
    }

    if (codeSnippetID != SkBuiltInCodeSnippetID::kDepthStencilOnlyDraw) {
        result->setWritesColor();
    }

    return blockSize;
}

void SkPaintParamsKey::toShaderInfo(SkShaderCodeDictionary* dict, SkShaderInfo* result) const {

    int curHeaderOffset = 0;
    while (curHeaderOffset < this->sizeInBytes()) {
        int blockSize = AddBlockToShaderInfo(dict, *this, curHeaderOffset, result);
        curHeaderOffset += blockSize;
    }
}

#if GR_TEST_UTILS
bool SkPaintParamsKey::isErrorKey() const {
    return this->sizeInBytes() == SkPaintParamsKey::kBlockHeaderSizeInBytes &&
           fData[0] == static_cast<int>(SkBuiltInCodeSnippetID::kError) &&
           fData[1] == SkPaintParamsKey::kBlockHeaderSizeInBytes;
}
#endif
