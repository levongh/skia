#include "SkNativeParsedPDF.h"
#include "SkPdfNativeTokenizer.h"
#include "SkPdfBasics.h"
#include "SkPdfParser.h"
#include "SkPdfObject.h"

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "SkPdfFileTrailerDictionary_autogen.h"
#include "SkPdfCatalogDictionary_autogen.h"
#include "SkPdfPageObjectDictionary_autogen.h"
#include "SkPdfPageTreeNodeDictionary_autogen.h"
#include "SkPdfMapper_autogen.h"



long getFileSize(const char* filename)
{
    struct stat stat_buf;
    int rc = stat(filename, &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

unsigned char* lineHome(unsigned char* start, unsigned char* current) {
    while (current > start && !isPdfEOL(*(current - 1))) {
        current--;
    }
    return current;
}

unsigned char* previousLineHome(unsigned char* start, unsigned char* current) {
    if (current > start && isPdfEOL(*(current - 1))) {
        current--;
    }

    // allows CR+LF, LF+CR but not two CR+CR or LF+LF
    if (current > start && isPdfEOL(*(current - 1)) && *current != *(current - 1)) {
        current--;
    }

    while (current > start && !isPdfEOL(*(current - 1))) {
        current--;
    }

    return current;
}

unsigned char* ignoreLine(unsigned char* current, unsigned char* end) {
    while (current < end && !isPdfEOL(*current)) {
        current++;
    }
    current++;
    if (current < end && isPdfEOL(*current) && *current != *(current - 1)) {
        current++;
    }
    return current;
}


// TODO(edisonn): NYI
// TODO(edisonn): 3 constructuctors from URL, from stream, from file ...
// TODO(edisonn): write one that accepts errors in the file and ignores/fixis them
// TODO(edisonn): testing:
// 1) run on a lot of file
// 2) recoverable corupt file: remove endobj, endsteam, remove other keywords, use other white spaces, insert comments randomly, ...
// 3) irrecoverable corrupt file
SkNativeParsedPDF::SkNativeParsedPDF(const char* path) : fAllocator(new SkPdfAllocator()) {
    FILE* file = fopen(path, "r");
    fContentLength = getFileSize(path);
    fFileContent = new unsigned char[fContentLength];
    fread(fFileContent, fContentLength, 1, file);
    fclose(file);
    file = NULL;

    unsigned char* eofLine = lineHome(fFileContent, fFileContent + fContentLength - 1);
    unsigned char* xrefByteOffsetLine = previousLineHome(fFileContent, eofLine);
    unsigned char* xrefstartKeywordLine = previousLineHome(fFileContent, xrefByteOffsetLine);

    if (strcmp((char*)xrefstartKeywordLine, "startxref") != 0) {
        // TODO(edisonn): report/issue
    }

    long xrefByteOffset = atol((const char*)xrefByteOffsetLine);

    bool storeCatalog = true;
    while (xrefByteOffset >= 0) {
        unsigned char* trailerStart = readCrossReferenceSection(fFileContent + xrefByteOffset, xrefstartKeywordLine);
        xrefByteOffset = readTrailer(trailerStart, xrefstartKeywordLine, storeCatalog);
        storeCatalog = false;
    }

    // TODO(edisonn): warn/error expect fObjects[fRefCatalogId].fGeneration == fRefCatalogGeneration
    // TODO(edisonn): security, verify that SkPdfCatalogDictionary is indeed using mapper
    // load catalog
    fRootCatalog = (SkPdfCatalogDictionary*)resolveReference(fRootCatalogRef);
    SkPdfPageTreeNodeDictionary* tree = fRootCatalog->Pages(this);

    fillPages(tree);

    // now actually read all objects if we want, or do it lazyly
    // and resolve references?... or not ...
}

// TODO(edisonn): NYI
SkNativeParsedPDF::~SkNativeParsedPDF() {
    delete[] fFileContent;
    delete fAllocator;
}

unsigned char* SkNativeParsedPDF::readCrossReferenceSection(unsigned char* xrefStart, unsigned char* trailerEnd) {
    unsigned char* current = ignoreLine(xrefStart, trailerEnd);  // TODO(edisonn): verify next keyord is "xref", use nextObject here

    SkPdfObject token;
    while (current < trailerEnd) {
        token.reset();
        unsigned char* previous = current;
        current = nextObject(current, trailerEnd, &token, NULL);
        if (!token.isInteger()) {
            return previous;
        }

        int startId = token.intValue();
        token.reset();
        current = nextObject(current, trailerEnd, &token, NULL);

        if (!token.isInteger()) {
            // TODO(edisonn): report/warning
            return current;
        }

        int entries = token.intValue();

        for (int i = 0; i < entries; i++) {
            token.reset();
            current = nextObject(current, trailerEnd, &token, NULL);
            if (!token.isInteger()) {
                // TODO(edisonn): report/warning
                return current;
            }
            int offset = token.intValue();

            token.reset();
            current = nextObject(current, trailerEnd, &token, NULL);
            if (!token.isInteger()) {
                // TODO(edisonn): report/warning
                return current;
            }
            int generation = token.intValue();

            token.reset();
            current = nextObject(current, trailerEnd, &token, NULL);
            if (!token.isKeyword() || token.len() != 1 || (*token.c_str() != 'f' && *token.c_str() != 'n')) {
                // TODO(edisonn): report/warning
                return current;
            }

            addCrossSectionInfo(startId + i, generation, offset, *token.c_str() == 'f');
        }
    }
    // TODO(edisonn): it should never get here? there is no trailer?
    return current;
}

long SkNativeParsedPDF::readTrailer(unsigned char* trailerStart, unsigned char* trailerEnd, bool storeCatalog) {
    unsigned char* current = ignoreLine(trailerStart, trailerEnd);  // TODO(edisonn): verify next keyord is "trailer" use nextObject here

    SkPdfObject token;
    current = nextObject(current, trailerEnd, &token, fAllocator);
    SkPdfFileTrailerDictionary* trailer = (SkPdfFileTrailerDictionary*)&token;

    if (storeCatalog) {
        const SkPdfObject* ref = trailer->Root(NULL);
        if (ref == NULL || !ref->isReference()) {
            // TODO(edisonn): oops, we have to fix the corrup pdf file
            return -1;
        }
        fRootCatalogRef = ref;
    }

    if (trailer->has_Prev()) {
        return trailer->Prev(NULL);
    }

    return -1;
}

void SkNativeParsedPDF::addCrossSectionInfo(int id, int generation, int offset, bool isFreed) {
    // TODO(edisonn): security here
    while (fObjects.count() < id + 1) {
        reset(fObjects.append());
    }

    fObjects[id].fOffset = offset;
    fObjects[id].fObj = NULL;
}

SkPdfObject* SkNativeParsedPDF::readObject(int id/*, int expectedGeneration*/) const {
    long startOffset = fObjects[id].fOffset;
    //long endOffset = fObjects[id].fOffsetEnd;
    // TODO(edisonn): use hinted endOffset
    // TODO(edisonn): current implementation will result in a lot of memory usage
    // to decrease memory usage, we wither need to be smart and know where objects end, and we will
    // alocate only the chancks needed, or the tokenizer will not make copies, but then it needs to
    // cache the results so it does not go twice on the same buffer
    unsigned char* current = fFileContent + startOffset;
    unsigned char* end = fFileContent + fContentLength;

    SkPdfNativeTokenizer tokenizer(current, end - current, fMapper, fAllocator);

    SkPdfObject idObj;
    SkPdfObject generationObj;
    SkPdfObject objKeyword;
    SkPdfObject* dict = fAllocator->allocObject();

    current = nextObject(current, end, &idObj, NULL);
    if (current >= end) {
        // TODO(edisonn): report warning/error
        return NULL;
    }

    current = nextObject(current, end, &generationObj, NULL);
    if (current >= end) {
        // TODO(edisonn): report warning/error
        return NULL;
    }

    current = nextObject(current, end, &objKeyword, NULL);
    if (current >= end) {
        // TODO(edisonn): report warning/error
        return NULL;
    }

    if (!idObj.isInteger() || !generationObj.isInteger() || id != idObj.intValue()/* || generation != generationObj.intValue()*/) {
        // TODO(edisonn): report warning/error
    }

    if (!objKeyword.isKeyword() || strcmp(objKeyword.c_str(), "obj") != 0) {
        // TODO(edisonn): report warning/error
    }

    current = nextObject(current, end, dict, fAllocator);

    // TODO(edisonn): report warning/error - verify last token is endobj

    return dict;
}

void SkNativeParsedPDF::fillPages(SkPdfPageTreeNodeDictionary* tree) {
    const SkPdfArray* kids = tree->Kids(this);
    if (kids == NULL) {
        *fPages.append() = (SkPdfPageObjectDictionary*)tree;
        return;
    }

    int cnt = kids->size();
    for (int i = 0; i < cnt; i++) {
        const SkPdfObject* obj = resolveReference(kids->objAtAIndex(i));
        if (fMapper->mapPageObjectDictionary(obj) != kPageObjectDictionary_SkPdfObjectType) {
            *fPages.append() = (SkPdfPageObjectDictionary*)obj;
        } else {
            // TODO(edisonn): verify that it is a page tree indeed
            fillPages((SkPdfPageTreeNodeDictionary*)obj);
        }
    }
}

int SkNativeParsedPDF::pages() const {
    return fPages.count();
}

SkPdfResourceDictionary* SkNativeParsedPDF::pageResources(int page) {
    return fPages[page]->Resources(this);
}

// TODO(edisonn): Partial implemented. Move the logics directly in the code generator for inheritable and default value?
SkRect SkNativeParsedPDF::MediaBox(int page) const {
    SkPdfPageObjectDictionary* current = fPages[page];
    while (!current->has_MediaBox() && current->has_Parent()) {
        current = (SkPdfPageObjectDictionary*)current->Parent(this);
    }
    if (current) {
        return current->MediaBox(this);
    }
    return SkRect::MakeEmpty();
}

// TODO(edisonn): stream or array ... ? for now only array
SkPdfNativeTokenizer* SkNativeParsedPDF::tokenizerOfPage(int page) const {
    if (fPages[page]->isContentsAStream(this)) {
        return tokenizerOfStream(fPages[page]->getContentsAsStream(this));
    } else {
        // TODO(edisonn): NYI, we need to concatenate all streams in the array or make the tokenizer smart
        // so we don't allocate new memory
        return NULL;
    }
}

SkPdfNativeTokenizer* SkNativeParsedPDF::tokenizerOfStream(SkPdfObject* stream) const {
    if (stream == NULL) {
        return NULL;
    }

    return new SkPdfNativeTokenizer(stream, fMapper, fAllocator);
}

// TODO(edisonn): NYI
SkPdfNativeTokenizer* SkNativeParsedPDF::tokenizerOfBuffer(unsigned char* buffer, size_t len) const {
    // warning does not track two calls in the same buffer! the buffer is updated!
    // make a clean copy if needed!
    return new SkPdfNativeTokenizer(buffer, len, fMapper, fAllocator);
}

size_t SkNativeParsedPDF::objects() const {
    return fObjects.count();
}

SkPdfObject* SkNativeParsedPDF::object(int i) {
    SkASSERT(!(i < 0 || i > fObjects.count()));

    if (i < 0 || i > fObjects.count()) {
        return NULL;
    }

    if (fObjects[i].fObj == NULL) {
        // TODO(edisonn): when we read the cross reference sections, store the start of the next object
        // and fill fOffsetEnd
        fObjects[i].fObj = readObject(i);
    }

    return fObjects[i].fObj;
}

const SkPdfMapper* SkNativeParsedPDF::mapper() const {
    return fMapper;
}

SkPdfReal* SkNativeParsedPDF::createReal(double value) const {
    SkPdfObject* obj = fAllocator->allocObject();
    SkPdfObject::makeReal(value, obj);
    return (SkPdfReal*)obj;
}

SkPdfInteger* SkNativeParsedPDF::createInteger(int value) const {
    SkPdfObject* obj = fAllocator->allocObject();
    SkPdfObject::makeInteger(value, obj);
    return (SkPdfInteger*)obj;
}

SkPdfString* SkNativeParsedPDF::createString(unsigned char* sz, size_t len) const {
    SkPdfObject* obj = fAllocator->allocObject();
    SkPdfObject::makeString(sz, len, obj);
    return (SkPdfString*)obj;
}

PdfContext* gPdfContext = NULL;

void SkNativeParsedPDF::drawPage(int page, SkCanvas* canvas) {
    SkPdfNativeTokenizer* tokenizer = tokenizerOfPage(page);

    PdfContext pdfContext(this);
    pdfContext.fOriginalMatrix = SkMatrix::I();
    pdfContext.fGraphicsState.fResources = pageResources(page);

    gPdfContext = &pdfContext;

    // TODO(edisonn): get matrix stuff right.
    // TODO(edisonn): add DPI/scale/zoom.
    SkScalar z = SkIntToScalar(0);
    SkRect rect = MediaBox(page);
    SkScalar w = rect.width();
    SkScalar h = rect.height();

    SkPoint pdfSpace[4] = {SkPoint::Make(z, z), SkPoint::Make(w, z), SkPoint::Make(w, h), SkPoint::Make(z, h)};
//                SkPoint skiaSpace[4] = {SkPoint::Make(z, h), SkPoint::Make(w, h), SkPoint::Make(w, z), SkPoint::Make(z, z)};

    // TODO(edisonn): add flag for this app to create sourunding buffer zone
    // TODO(edisonn): add flagg for no clipping.
    // Use larger image to make sure we do not draw anything outside of page
    // could be used in tests.

#ifdef PDF_DEBUG_3X
    SkPoint skiaSpace[4] = {SkPoint::Make(w+z, h+h), SkPoint::Make(w+w, h+h), SkPoint::Make(w+w, h+z), SkPoint::Make(w+z, h+z)};
#else
    SkPoint skiaSpace[4] = {SkPoint::Make(z, h), SkPoint::Make(w, h), SkPoint::Make(w, z), SkPoint::Make(z, z)};
#endif
    //SkPoint pdfSpace[2] = {SkPoint::Make(z, z), SkPoint::Make(w, h)};
    //SkPoint skiaSpace[2] = {SkPoint::Make(w, z), SkPoint::Make(z, h)};

    //SkPoint pdfSpace[2] = {SkPoint::Make(z, z), SkPoint::Make(z, h)};
    //SkPoint skiaSpace[2] = {SkPoint::Make(z, h), SkPoint::Make(z, z)};

    //SkPoint pdfSpace[3] = {SkPoint::Make(z, z), SkPoint::Make(z, h), SkPoint::Make(w, h)};
    //SkPoint skiaSpace[3] = {SkPoint::Make(z, h), SkPoint::Make(z, z), SkPoint::Make(w, 0)};

    SkAssertResult(pdfContext.fOriginalMatrix.setPolyToPoly(pdfSpace, skiaSpace, 4));
    SkTraceMatrix(pdfContext.fOriginalMatrix, "Original matrix");


    pdfContext.fGraphicsState.fMatrix = pdfContext.fOriginalMatrix;
    pdfContext.fGraphicsState.fMatrixTm = pdfContext.fGraphicsState.fMatrix;
    pdfContext.fGraphicsState.fMatrixTlm = pdfContext.fGraphicsState.fMatrix;

    canvas->setMatrix(pdfContext.fOriginalMatrix);

#ifndef PDF_DEBUG_NO_PAGE_CLIPING
    canvas->clipRect(SkRect::MakeXYWH(z, z, w, h), SkRegion::kIntersect_Op, true);
#endif

// erase with red before?
//        SkPaint paint;
//        paint.setColor(SK_ColorRED);
//        canvas->drawRect(rect, paint);

    PdfMainLooper looper(NULL, tokenizer, &pdfContext, canvas);
    looper.loop();

    delete tokenizer;

    canvas->flush();
}

SkPdfAllocator* SkNativeParsedPDF::allocator() const {
    return fAllocator;
}

SkPdfObject* SkNativeParsedPDF::resolveReference(SkPdfObject* ref) const {
    return (SkPdfObject*)resolveReference((const SkPdfObject*)ref);
}

// TODO(edisonn): fix infinite loop if ref to itself!
// TODO(edisonn): perf, fix refs at load, and resolve will simply return fResolvedReference?
SkPdfObject* SkNativeParsedPDF::resolveReference(const SkPdfObject* ref) const {
    if (ref && ref->isReference()) {
        int id = ref->referenceId();
        // TODO(edisonn): generation/updates not supported now
        //int gen = ref->referenceGeneration();

        SkASSERT(!(id < 0 || id > fObjects.count()));

        if (id < 0 || id > fObjects.count()) {
            return NULL;
        }

        // TODO(edisonn): verify id and gen expected

        if (fObjects[id].fResolvedReference != NULL) {
            return fObjects[id].fResolvedReference;
        }

        if (fObjects[id].fObj == NULL) {
            fObjects[id].fObj = readObject(id);
        }

        if (fObjects[id].fResolvedReference == NULL) {
            if (!fObjects[id].fObj->isReference()) {
                fObjects[id].fResolvedReference = fObjects[id].fObj;
            } else {
                fObjects[id].fResolvedReference = resolveReference(fObjects[id].fObj);
            }
        }

        return fObjects[id].fResolvedReference;
    }
    // TODO(edisonn): fix the mess with const, probably we need to remove it pretty much everywhere
    return (SkPdfObject*)ref;
}