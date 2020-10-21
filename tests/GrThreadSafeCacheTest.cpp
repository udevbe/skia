/*
 * Copyright 2020 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkCanvas.h"
#include "include/core/SkDeferredDisplayListRecorder.h"
#include "include/core/SkSurfaceCharacterization.h"
#include "src/gpu/GrDirectContextPriv.h"
#include "src/gpu/GrProxyProvider.h"
#include "src/gpu/GrRecordingContextPriv.h"
#include "src/gpu/GrRenderTargetContext.h"
#include "src/gpu/GrStyle.h"
#include "src/gpu/GrThreadSafeCache.h"
#include "tests/Test.h"
#include "tests/TestUtils.h"

#include <thread>

static constexpr int kImageWH = 32;
static constexpr auto kImageOrigin = kBottomLeft_GrSurfaceOrigin;
static constexpr int kNoID = -1;

static SkImageInfo default_ii(int wh) {
    return SkImageInfo::Make(wh, wh, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
}

static std::unique_ptr<GrRenderTargetContext> new_RTC(GrRecordingContext* rContext, int wh) {
    return GrRenderTargetContext::Make(rContext,
                                       GrColorType::kRGBA_8888,
                                       nullptr,
                                       SkBackingFit::kExact,
                                       {wh, wh},
                                       1,
                                       GrMipMapped::kNo,
                                       GrProtected::kNo,
                                       kImageOrigin,
                                       SkBudgeted::kYes);
}

static void create_key(GrUniqueKey* key, int wh, int id) {
    static const GrUniqueKey::Domain kDomain = GrUniqueKey::GenerateDomain();
    GrUniqueKey::Builder builder(key, kDomain, 1);
    builder[0] = wh;
    builder.finish();

    if (id != kNoID) {
        key->setCustomData(SkData::MakeWithCopy(&id, sizeof(id)));
    }
};

static SkBitmap create_bitmap(int wh) {
    SkBitmap bitmap;

    bitmap.allocPixels(default_ii(wh));

    SkCanvas tmp(bitmap);
    tmp.clear(SK_ColorWHITE);

    SkPaint blue;
    blue.setColor(SK_ColorBLUE);
    blue.setAntiAlias(false);

    tmp.drawRect({10, 10, wh-10.0f, wh-10.0f}, blue);

    bitmap.setImmutable();
    return bitmap;
}

class TestHelper {
public:
    struct Stats {
        int fCacheHits = 0;
        int fCacheMisses = 0;

        int fNumSWCreations = 0;
        int fNumLazyCreations = 0;
        int fNumHWCreations = 0;
    };

    TestHelper(GrDirectContext* dContext) : fDContext(dContext) {

        fDst = SkSurface::MakeRenderTarget(dContext, SkBudgeted::kNo, default_ii(kImageWH));
        SkAssertResult(fDst);

        SkSurfaceCharacterization characterization;
        SkAssertResult(fDst->characterize(&characterization));

        fRecorder1 = std::make_unique<SkDeferredDisplayListRecorder>(characterization);
        this->ddlCanvas1()->clear(SkColors::kGreen);

        fRecorder2 = std::make_unique<SkDeferredDisplayListRecorder>(characterization);
        this->ddlCanvas2()->clear(SkColors::kRed);
    }

    ~TestHelper() {
        fDContext->flush();
        fDContext->submit(true);
    }

    Stats* stats() { return &fStats; }

    int numCacheEntries() const { return this->threadSafeCache()->numEntries(); }

    GrDirectContext* dContext() { return fDContext; }

    SkCanvas* liveCanvas() { return fDst ? fDst->getCanvas() : nullptr; }
    SkCanvas* ddlCanvas1() { return fRecorder1 ? fRecorder1->getCanvas() : nullptr; }
    sk_sp<SkDeferredDisplayList> snap1() {
        if (fRecorder1) {
            sk_sp<SkDeferredDisplayList> tmp = fRecorder1->detach();
            fRecorder1 = nullptr;
            return tmp;
        }

        return nullptr;
    }
    SkCanvas* ddlCanvas2() { return fRecorder2 ? fRecorder2->getCanvas() : nullptr; }
    sk_sp<SkDeferredDisplayList> snap2() {
        if (fRecorder2) {
            sk_sp<SkDeferredDisplayList> tmp = fRecorder2->detach();
            fRecorder2 = nullptr;
            return tmp;
        }

        return nullptr;
    }

    GrThreadSafeCache* threadSafeCache() { return fDContext->priv().threadSafeCache(); }
    const GrThreadSafeCache* threadSafeCache() const { return fDContext->priv().threadSafeCache(); }

    // Add a draw on 'canvas' that will introduce a ref on the 'wh' view
    void accessCachedView(SkCanvas* canvas,
                          int wh,
                          int id = kNoID,
                          bool failLookup = false,
                          bool failFillingIn = false) {
        GrRecordingContext* rContext = canvas->recordingContext();

        auto view = AccessCachedView(rContext, this->threadSafeCache(),
                                     wh, failLookup, failFillingIn, id, &fStats);
        SkASSERT(view);

        auto rtc = canvas->internal_private_accessTopLayerRenderTargetContext();

        rtc->drawTexture(nullptr,
                         view,
                         kPremul_SkAlphaType,
                         GrSamplerState::Filter::kNearest,
                         GrSamplerState::MipmapMode::kNone,
                         SkBlendMode::kSrcOver,
                         {1.0f, 1.0f, 1.0f, 1.0f},
                         SkRect::MakeWH(wh, wh),
                         SkRect::MakeWH(wh, wh),
                         GrAA::kNo,
                         GrQuadAAFlags::kNone,
                         SkCanvas::kFast_SrcRectConstraint,
                         SkMatrix::I(),
                         nullptr);
    }

    // Besides checking that the number of refs and cache hits and misses are as expected, this
    // method also validates that the unique key doesn't appear in any of the other caches.
    bool checkView(SkCanvas* canvas, int wh, int hits, int misses, int numRefs, int expectedID) {
        if (fStats.fCacheHits != hits || fStats.fCacheMisses != misses) {
            SkDebugf("Hits E: %d A: %d --- Misses E: %d A: %d\n",
                     hits, fStats.fCacheHits, misses, fStats.fCacheMisses);
            return false;
        }

        GrUniqueKey key;
        create_key(&key, wh, kNoID);

        auto threadSafeCache = this->threadSafeCache();

        auto [view, data] = threadSafeCache->findWithData(key);
        if (!view.proxy()) {
            return false;
        }

        if (expectedID < 0) {
            if (data) {
                return false;
            }
        } else {
            if (!data) {
                return false;
            }

            const int* cachedID = static_cast<const int*>(data->data());
            if (*cachedID != expectedID) {
                return false;
            }
        }

        if (!view.proxy()->refCntGreaterThan(numRefs+1) ||  // +1 for 'view's ref
            view.proxy()->refCntGreaterThan(numRefs+2)) {
            return false;
        }

        if (canvas) {
            GrRecordingContext* rContext = canvas->recordingContext();
            GrProxyProvider* recordingProxyProvider = rContext->priv().proxyProvider();
            sk_sp<GrTextureProxy> result = recordingProxyProvider->findProxyByUniqueKey(key);
            if (result) {
                // views in this cache should never appear in the recorder's cache
                return false;
            }
        }

        {
            GrProxyProvider* directProxyProvider = fDContext->priv().proxyProvider();
            sk_sp<GrTextureProxy> result = directProxyProvider->findProxyByUniqueKey(key);
            if (result) {
                // views in this cache should never appear in the main proxy cache
                return false;
            }
        }

        {
            auto resourceProvider = fDContext->priv().resourceProvider();
            sk_sp<GrSurface> surf = resourceProvider->findByUniqueKey<GrSurface>(key);
            if (surf) {
                // the textures backing the views in this cache should never be discoverable in the
                // resource cache
                return false;
            }
        }

        return true;
    }

    bool checkImage(skiatest::Reporter* reporter, sk_sp<SkSurface> s) {
        SkBitmap actual;

        actual.allocPixels(default_ii(kImageWH));

        if (!s->readPixels(actual, 0, 0)) {
            return false;
        }

        SkBitmap expected = create_bitmap(kImageWH);

        const float tols[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        auto error = std::function<ComparePixmapsErrorReporter>(
            [reporter](int x, int y, const float diffs[4]) {
                SkASSERT(x >= 0 && y >= 0);
                ERRORF(reporter, "mismatch at %d, %d (%f, %f, %f %f)",
                       x, y, diffs[0], diffs[1], diffs[2], diffs[3]);
            });

        return ComparePixels(expected.pixmap(), actual.pixmap(), tols, error);
    }

    bool checkImage(skiatest::Reporter* reporter) {
        return this->checkImage(reporter, fDst);
    }

    bool checkImage(skiatest::Reporter* reporter, sk_sp<SkDeferredDisplayList> ddl) {
        sk_sp<SkSurface> tmp = SkSurface::MakeRenderTarget(fDContext,
                                                           SkBudgeted::kNo,
                                                           default_ii(kImageWH));
        if (!tmp) {
            return false;
        }

        if (!tmp->draw(std::move(ddl))) {
            return false;
        }

        return this->checkImage(reporter, std::move(tmp));
    }

    size_t gpuSize(int wh) const {
        GrBackendFormat format = fDContext->defaultBackendFormat(kRGBA_8888_SkColorType,
                                                                 GrRenderable::kNo);

        return GrSurface::ComputeSize(format, {wh, wh}, /*colorSamplesPerPixel=*/1,
                                      GrMipMapped::kNo, /*binSize=*/false);
    }

private:
    static GrSurfaceProxyView AccessCachedView(GrRecordingContext*,
                                               GrThreadSafeCache*,
                                               int wh,
                                               bool failLookup, bool failFillingIn, int id,
                                               Stats*);
    static GrSurfaceProxyView CreateViewOnCpu(GrRecordingContext*, int wh, Stats*);
    static bool FillInViewOnGpu(GrDirectContext*, int wh, Stats*,
                                const GrSurfaceProxyView& lazyView,
                                sk_sp<GrThreadSafeCache::Trampoline>);

    Stats fStats;
    GrDirectContext* fDContext = nullptr;

    sk_sp<SkSurface> fDst;
    std::unique_ptr<SkDeferredDisplayListRecorder> fRecorder1;
    std::unique_ptr<SkDeferredDisplayListRecorder> fRecorder2;
};

GrSurfaceProxyView TestHelper::CreateViewOnCpu(GrRecordingContext* rContext,
                                               int wh,
                                               Stats* stats) {
    GrProxyProvider* proxyProvider = rContext->priv().proxyProvider();

    sk_sp<GrTextureProxy> proxy = proxyProvider->createProxyFromBitmap(create_bitmap(wh),
                                                                       GrMipmapped::kNo,
                                                                       SkBackingFit::kExact,
                                                                       SkBudgeted::kYes);
    if (!proxy) {
        return {};
    }

    GrSwizzle swizzle = rContext->priv().caps()->getReadSwizzle(proxy->backendFormat(),
                                                                GrColorType::kRGBA_8888);
    ++stats->fNumSWCreations;
    return {std::move(proxy), kImageOrigin, swizzle};
}

bool TestHelper::FillInViewOnGpu(GrDirectContext* dContext, int wh, Stats* stats,
                                 const GrSurfaceProxyView& lazyView,
                                 sk_sp<GrThreadSafeCache::Trampoline> trampoline) {

    std::unique_ptr<GrRenderTargetContext> rtc = new_RTC(dContext, wh);

    GrPaint paint;
    paint.setColor4f({0.0f, 0.0f, 1.0f, 1.0f});

    rtc->clear({1.0f, 1.0f, 1.0f, 1.0f});
    rtc->drawRect(nullptr, std::move(paint), GrAA::kNo, SkMatrix::I(),
                  { 10, 10, wh-10.0f, wh-10.0f }, &GrStyle::SimpleFill());

    ++stats->fNumHWCreations;
    auto view = rtc->readSurfaceView();

    SkASSERT(view.swizzle() == lazyView.swizzle());
    SkASSERT(view.origin() == lazyView.origin());
    trampoline->fProxy = view.asTextureProxyRef();

    return true;
}

GrSurfaceProxyView TestHelper::AccessCachedView(GrRecordingContext* rContext,
                                                GrThreadSafeCache* threadSafeCache,
                                                int wh,
                                                bool failLookup, bool failFillingIn, int id,
                                                Stats* stats) {
    GrUniqueKey key;
    create_key(&key, wh, id);

    if (GrDirectContext* dContext = rContext->asDirectContext()) {
        // The gpu thread gets priority over the recording threads. If the gpu thread is first,
        // it crams a lazy proxy into the cache and then fills it in later.
        auto [lazyView, trampoline] = GrThreadSafeCache::CreateLazyView(
            dContext, GrColorType::kRGBA_8888, {wh, wh}, kImageOrigin, SkBackingFit::kExact);
        ++stats->fNumLazyCreations;

        auto [view, data] = threadSafeCache->findOrAddWithData(key, lazyView);
        if (view != lazyView) {
            ++stats->fCacheHits;
            return view;
        } else if (id != kNoID) {
            // Make sure, in this case, that the customData stuck
            SkASSERT(data);
            SkDEBUGCODE(const int* cachedID = static_cast<const int*>(data->data());)
            SkASSERT(*cachedID == id);
        }

        ++stats->fCacheMisses;

        if (failFillingIn) {
            // Simulate something going horribly wrong at flush-time so no GrTexture is
            // available to fulfill the lazy proxy.
            return view;
        }

        if (!FillInViewOnGpu(dContext, wh, stats, lazyView, std::move(trampoline))) {
            // In this case something has gone disastrously wrong so set up to drop the draw
            // that needed this resource and reduce future pollution of the cache.
            threadSafeCache->remove(key);
            return {};
        }

        return view;
    } else {
        GrSurfaceProxyView view;

        // We can "fail the lookup" to simulate a threaded race condition
        if (view = threadSafeCache->find(key); !failLookup && view) {
            ++stats->fCacheHits;
            return view;
        }

        ++stats->fCacheMisses;

        view = CreateViewOnCpu(rContext, wh, stats);
        SkASSERT(view);

        auto [newView, data] = threadSafeCache->addWithData(key, view);
        if (view == newView && id != kNoID) {
            // Make sure, in this case, that the customData stuck
            SkASSERT(data);
            SkDEBUGCODE(const int* cachedID = static_cast<const int*>(data->data());)
            SkASSERT(*cachedID == id);
        }
        return newView;
    }
}

// Case 1: ensure two DDL recorders share the view
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache1, reporter, ctxInfo) {
    TestHelper helper(ctxInfo.directContext());

    helper.accessCachedView(helper.ddlCanvas1(), kImageWH, 1);
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas1(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, /*id*/ 1));

    helper.accessCachedView(helper.ddlCanvas2(), kImageWH, 2);
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas2(), kImageWH,
                                               /*hits*/ 1, /*misses*/ 1, /*refs*/ 2, /*id*/ 1));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumLazyCreations == 0);
    REPORTER_ASSERT(reporter, helper.stats()->fNumHWCreations == 0);
    REPORTER_ASSERT(reporter, helper.stats()->fNumSWCreations == 1);

    helper.checkImage(reporter, helper.snap1());
    helper.checkImage(reporter, helper.snap2());
}

// Case 2: ensure that, if the direct context version wins, it is reused by the DDL recorders
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache2, reporter, ctxInfo) {
    TestHelper helper(ctxInfo.directContext());

    helper.accessCachedView(helper.liveCanvas(), kImageWH, 1);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, /*id*/ 1));

    helper.accessCachedView(helper.ddlCanvas1(), kImageWH, 2);
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas1(), kImageWH,
                                               /*hits*/ 1, /*misses*/ 1, /*refs*/ 2, /*id*/ 1));

    helper.accessCachedView(helper.ddlCanvas2(), kImageWH, 3);
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas2(), kImageWH,
                                               /*hits*/ 2, /*misses*/ 1, /*refs*/ 3, /*id*/ 1));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumLazyCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumHWCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumSWCreations == 0);

    helper.checkImage(reporter);
    helper.checkImage(reporter, helper.snap1());
    helper.checkImage(reporter, helper.snap2());
}

// Case 3: ensure that, if the cpu-version wins, it is reused by the direct context
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache3, reporter, ctxInfo) {
    TestHelper helper(ctxInfo.directContext());

    helper.accessCachedView(helper.ddlCanvas1(), kImageWH, 1);
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas1(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, /*id*/ 1));

    helper.accessCachedView(helper.liveCanvas(), kImageWH, 2);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 1, /*misses*/ 1, /*refs*/ 2, /*id*/ 1));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumLazyCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumHWCreations == 0);
    REPORTER_ASSERT(reporter, helper.stats()->fNumSWCreations == 1);

    helper.checkImage(reporter);
    helper.checkImage(reporter, helper.snap1());
}

// Case 4: ensure that, if two DDL recorders get in a race, they still end up sharing a single view
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache4, reporter, ctxInfo) {
    TestHelper helper(ctxInfo.directContext());

    helper.accessCachedView(helper.ddlCanvas1(), kImageWH, 1);
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas1(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, /*id*/ 1));

    static const bool kFailLookup = true;
    helper.accessCachedView(helper.ddlCanvas2(), kImageWH, 2, kFailLookup);
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas2(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 2, /*refs*/ 2, /*id*/ 1));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumLazyCreations == 0);
    REPORTER_ASSERT(reporter, helper.stats()->fNumHWCreations == 0);
    REPORTER_ASSERT(reporter, helper.stats()->fNumSWCreations == 2);

    helper.checkImage(reporter, helper.snap1());
    helper.checkImage(reporter, helper.snap2());
}

// Case 4.5: check that, if a live rendering and a DDL recording get into a race, the live
// rendering takes precedence.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache4_5, reporter, ctxInfo) {
    TestHelper helper(ctxInfo.directContext());

    helper.accessCachedView(helper.liveCanvas(), kImageWH, 1);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, /*id*/ 1));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumLazyCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumHWCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumSWCreations == 0);

    static const bool kFailLookup = true;
    helper.accessCachedView(helper.ddlCanvas1(), kImageWH, 2, kFailLookup);
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas1(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 2, /*refs*/ 2, /*id*/ 1));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumLazyCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumHWCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumSWCreations == 1);

    helper.checkImage(reporter);
    helper.checkImage(reporter, helper.snap1());
}

// Case 4.75: check that, if a live rendering fails to generate the content needed to instantiate
//            its lazy proxy, life goes on
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache4_75, reporter, ctxInfo) {
    auto dContext = ctxInfo.directContext();

    TestHelper helper(dContext);

    static const bool kFailFillingIn = true;
    helper.accessCachedView(helper.liveCanvas(), kImageWH, kNoID, false, kFailFillingIn);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, kNoID));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumLazyCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumHWCreations == 0);
    REPORTER_ASSERT(reporter, helper.stats()->fNumSWCreations == 0);

    dContext->flush();
    dContext->submit(true);

    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 0, kNoID));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumLazyCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumHWCreations == 0);
    REPORTER_ASSERT(reporter, helper.stats()->fNumSWCreations == 0);
}

// Case 5: ensure that expanding the map works (esp. wrt custom data)
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache5, reporter, ctxInfo) {
    TestHelper helper(ctxInfo.directContext());

    auto threadSafeCache = helper.threadSafeCache();

    int size = 16;
    helper.accessCachedView(helper.ddlCanvas1(), size, /*id*/ size);

    size_t initialSize = threadSafeCache->approxBytesUsedForHash();

    while (initialSize == threadSafeCache->approxBytesUsedForHash()) {
        size *= 2;
        helper.accessCachedView(helper.ddlCanvas1(), size, /*id*/ size);
    }

    for (int i = 16; i <= size; i *= 2) {
        REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas1(),
                                                   /*wh*/ i,
                                                   /*hits*/ 0,
                                                   /*misses*/ threadSafeCache->numEntries(),
                                                   /*refs*/ 1,
                                                   /*id*/ i));
    }
}

// Case 6: Check on dropping refs. In particular, that the cache has its own ref to keep
// the backing resource alive and locked.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache6, reporter, ctxInfo) {
    TestHelper helper(ctxInfo.directContext());

    helper.accessCachedView(helper.ddlCanvas1(), kImageWH);
    sk_sp<SkDeferredDisplayList> ddl1 = helper.snap1();
    REPORTER_ASSERT(reporter, helper.checkView(nullptr, kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, kNoID));

    helper.accessCachedView(helper.ddlCanvas2(), kImageWH);
    sk_sp<SkDeferredDisplayList> ddl2 = helper.snap2();
    REPORTER_ASSERT(reporter, helper.checkView(nullptr, kImageWH,
                                               /*hits*/ 1, /*misses*/ 1, /*refs*/ 2, kNoID));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);

    ddl1 = nullptr;
    REPORTER_ASSERT(reporter, helper.checkView(nullptr, kImageWH,
                                               /*hits*/ 1, /*misses*/ 1, /*refs*/ 1, kNoID));

    ddl2 = nullptr;
    REPORTER_ASSERT(reporter, helper.checkView(nullptr, kImageWH,
                                               /*hits*/ 1, /*misses*/ 1, /*refs*/ 0, kNoID));

    // The cache still has its ref
    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);

    REPORTER_ASSERT(reporter, helper.checkView(nullptr, kImageWH,
                                               /*hits*/ 1, /*misses*/ 1, /*refs*/ 0, kNoID));
}

// Case 7: Check that invoking dropAllRefs and dropUniqueRefs directly works as expected; i.e.,
// dropAllRefs removes everything while dropUniqueRefs is more measured.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache7, reporter, ctxInfo) {
    TestHelper helper(ctxInfo.directContext());

    helper.accessCachedView(helper.ddlCanvas1(), kImageWH);
    sk_sp<SkDeferredDisplayList> ddl1 = helper.snap1();
    REPORTER_ASSERT(reporter, helper.checkView(nullptr, kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, kNoID));

    helper.accessCachedView(helper.ddlCanvas2(), 2*kImageWH);
    sk_sp<SkDeferredDisplayList> ddl2 = helper.snap2();
    REPORTER_ASSERT(reporter, helper.checkView(nullptr, 2*kImageWH,
                                               /*hits*/ 0, /*misses*/ 2, /*refs*/ 1, kNoID));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 2);

    helper.threadSafeCache()->dropUniqueRefs(nullptr);
    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 2);

    ddl1 = nullptr;

    helper.threadSafeCache()->dropUniqueRefs(nullptr);
    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);
    REPORTER_ASSERT(reporter, helper.checkView(nullptr, 2*kImageWH,
                                               /*hits*/ 0, /*misses*/ 2, /*refs*/ 1, kNoID));

    helper.threadSafeCache()->dropAllRefs();
    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 0);

    ddl2 = nullptr;
}

// Case 8: This checks that GrContext::abandonContext works as expected wrt the thread
//         safe cache. This simulates the case where we have one DDL that has finished
//         recording but one still recording when the abandonContext fires.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache8, reporter, ctxInfo) {
    TestHelper helper(ctxInfo.directContext());

    helper.accessCachedView(helper.liveCanvas(), kImageWH);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, kNoID));

    helper.accessCachedView(helper.ddlCanvas1(), kImageWH);
    sk_sp<SkDeferredDisplayList> ddl1 = helper.snap1();
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas1(), kImageWH,
                                               /*hits*/ 1, /*misses*/ 1, /*refs*/ 2, kNoID));

    helper.accessCachedView(helper.ddlCanvas2(), kImageWH);
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas2(), kImageWH,
                                               /*hits*/ 2, /*misses*/ 1, /*refs*/ 3, kNoID));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumLazyCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumHWCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumSWCreations == 0);

    ctxInfo.directContext()->abandonContext(); // This should exercise dropAllRefs

    sk_sp<SkDeferredDisplayList> ddl2 = helper.snap2();

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 0);

    ddl1 = nullptr;
    ddl2 = nullptr;
}

// Case 9: This checks that GrContext::releaseResourcesAndAbandonContext works as expected wrt
//         the thread safe cache. This simulates the case where we have one DDL that has finished
//         recording but one still recording when the releaseResourcesAndAbandonContext fires.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache9, reporter, ctxInfo) {
    TestHelper helper(ctxInfo.directContext());

    helper.accessCachedView(helper.liveCanvas(), kImageWH);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, kNoID));

    helper.accessCachedView(helper.ddlCanvas1(), kImageWH);
    sk_sp<SkDeferredDisplayList> ddl1 = helper.snap1();
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas1(), kImageWH,
                                               /*hits*/ 1, /*misses*/ 1, /*refs*/ 2, kNoID));

    helper.accessCachedView(helper.ddlCanvas2(), kImageWH);
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas2(), kImageWH,
                                               /*hits*/ 2, /*misses*/ 1, /*refs*/ 3, kNoID));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumLazyCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumHWCreations == 1);
    REPORTER_ASSERT(reporter, helper.stats()->fNumSWCreations == 0);

    ctxInfo.directContext()->releaseResourcesAndAbandonContext(); // This should hit dropAllRefs

    sk_sp<SkDeferredDisplayList> ddl2 = helper.snap2();

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 0);

    ddl1 = nullptr;
    ddl2 = nullptr;
}

// Case 10: This checks that the GrContext::purgeUnlockedResources(size_t) variant works as
//          expected wrt the thread safe cache. It, in particular, tests out the MRU behavior
//          of the shared cache.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache10, reporter, ctxInfo) {
    auto dContext = ctxInfo.directContext();

    if (GrBackendApi::kOpenGL != dContext->backend()) {
        // The lower-level backends have too much going on for the following simple purging
        // test to work
        return;
    }

    TestHelper helper(dContext);

    helper.accessCachedView(helper.liveCanvas(), kImageWH);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, kNoID));

    helper.accessCachedView(helper.ddlCanvas1(), kImageWH);
    sk_sp<SkDeferredDisplayList> ddl1 = helper.snap1();
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas1(), kImageWH,
                                               /*hits*/ 1, /*misses*/ 1, /*refs*/ 2, kNoID));

    helper.accessCachedView(helper.liveCanvas(), 2*kImageWH);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), 2*kImageWH,
                                               /*hits*/ 1, /*misses*/ 2, /*refs*/ 1, kNoID));

    helper.accessCachedView(helper.ddlCanvas2(), 2*kImageWH);
    sk_sp<SkDeferredDisplayList> ddl2 = helper.snap2();
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas2(), 2*kImageWH,
                                               /*hits*/ 2, /*misses*/ 2, /*refs*/ 2, kNoID));

    dContext->flush();
    dContext->submit(true);

    // This should clear out everything but the textures locked in the thread-safe cache
    dContext->purgeUnlockedResources(false);

    ddl1 = nullptr;
    ddl2 = nullptr;

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 2);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 2, /*misses*/ 2, /*refs*/ 0, kNoID));
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), 2*kImageWH,
                                               /*hits*/ 2, /*misses*/ 2, /*refs*/ 0, kNoID));

    // Regardless of which image is MRU, this should force the other out
    size_t desiredBytes = helper.gpuSize(2*kImageWH) + helper.gpuSize(kImageWH)/2;

    auto cache = dContext->priv().getResourceCache();
    size_t currentBytes = cache->getResourceBytes();

    SkASSERT(currentBytes >= desiredBytes);
    size_t amountToPurge = currentBytes - desiredBytes;

    // The 2*kImageWH texture should be MRU.
    dContext->purgeUnlockedResources(amountToPurge, true);

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);

    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), 2*kImageWH,
                                               /*hits*/ 2, /*misses*/ 2, /*refs*/ 0, kNoID));
}

// Case 11: This checks that scratch-only variant of GrContext::purgeUnlockedResources works as
//          expected wrt the thread safe cache. In particular, that when 'scratchResourcesOnly'
//          is true, the call has no effect on the cache.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache11, reporter, ctxInfo) {
    auto dContext = ctxInfo.directContext();

    TestHelper helper(dContext);

    helper.accessCachedView(helper.liveCanvas(), kImageWH);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, kNoID));

    helper.accessCachedView(helper.liveCanvas(), 2*kImageWH);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), 2*kImageWH,
                                               /*hits*/ 0, /*misses*/ 2, /*refs*/ 1, kNoID));

    dContext->flush();
    dContext->submit(true);

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 2);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 2, /*refs*/ 0, kNoID));
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), 2*kImageWH,
                                               /*hits*/ 0, /*misses*/ 2, /*refs*/ 0, kNoID));

    // This shouldn't remove anything from the cache
    dContext->purgeUnlockedResources(/* scratchResourcesOnly */ true);

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 2);

    dContext->purgeUnlockedResources(/* scratchResourcesOnly */ false);

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 0);
}

// Case 12: Test out purges caused by resetting the cache budget to 0. Note that, due to
//          the how the cache operates (i.e., not directly driven by ref/unrefs) there
//          needs to be an explicit kick to purge the cache.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache12, reporter, ctxInfo) {
    auto dContext = ctxInfo.directContext();

    TestHelper helper(dContext);

    helper.accessCachedView(helper.liveCanvas(), kImageWH);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, kNoID));
    helper.accessCachedView(helper.ddlCanvas1(), kImageWH);
    sk_sp<SkDeferredDisplayList> ddl1 = helper.snap1();
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas1(), kImageWH,
                                               /*hits*/ 1, /*misses*/ 1, /*refs*/ 2, kNoID));

    helper.accessCachedView(helper.liveCanvas(), 2*kImageWH);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), 2*kImageWH,
                                               /*hits*/ 1, /*misses*/ 2, /*refs*/ 1, kNoID));

    dContext->flush();
    dContext->submit(true);

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 2);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), kImageWH,
                                               /*hits*/ 1, /*misses*/ 2, /*refs*/ 1, kNoID));
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), 2*kImageWH,
                                               /*hits*/ 1, /*misses*/ 2, /*refs*/ 0, kNoID));

    dContext->setResourceCacheLimit(0);

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);

    ddl1 = nullptr;

    // Explicitly kick off the purge - it won't happen automatically on unref
    dContext->performDeferredCleanup(std::chrono::milliseconds(0));

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 0);
}

// Case 13: Test out the 'msNotUsed' parameter to GrContext::performDeferredCleanup.
DEF_GPUTEST_FOR_RENDERING_CONTEXTS(GrThreadSafeCache13, reporter, ctxInfo) {
    auto dContext = ctxInfo.directContext();

    TestHelper helper(dContext);

    helper.accessCachedView(helper.ddlCanvas1(), kImageWH);

    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas1(), kImageWH,
                                               /*hits*/ 0, /*misses*/ 1, /*refs*/ 1, kNoID));
    sk_sp<SkDeferredDisplayList> ddl1 = helper.snap1();

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto firstTime = GrStdSteadyClock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    helper.accessCachedView(helper.ddlCanvas2(), 2*kImageWH);
    REPORTER_ASSERT(reporter, helper.checkView(helper.ddlCanvas2(), 2*kImageWH,
                                               /*hits*/ 0, /*misses*/ 2, /*refs*/ 1, kNoID));
    sk_sp<SkDeferredDisplayList> ddl2 = helper.snap2();

    ddl1 = nullptr;
    ddl2 = nullptr;

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 2);

    auto secondTime = GrStdSteadyClock::now();

    auto msecs = std::chrono::duration_cast<std::chrono::milliseconds>(secondTime - firstTime);
    dContext->performDeferredCleanup(msecs);

    REPORTER_ASSERT(reporter, helper.numCacheEntries() == 1);
    REPORTER_ASSERT(reporter, helper.checkView(helper.liveCanvas(), 2*kImageWH,
                                               /*hits*/ 0, /*misses*/ 2, /*refs*/ 0, kNoID));
}
