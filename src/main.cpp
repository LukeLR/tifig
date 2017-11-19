#include <chrono>
#include <cxxopts.hpp>
#include <hevcimagefilereader.hpp>
#include <log.hpp>
#include <thread>
#include <future>
#include <vips/vips8>

extern "C" {
    #include <libavutil/opt.h>
    #include <libavutil/imgutils.h>
    #include <libavcodec/avcodec.h>
    #include <libswscale/swscale.h>
    #include <exif-loader.h>
};

using namespace std;
using namespace vips;

typedef ImageFileReaderInterface::DataVector DataVector;
typedef ImageFileReaderInterface::IdVector IdVector;
typedef ImageFileReaderInterface::GridItem GridItem;
typedef ImageFileReaderInterface::FileReaderException FileReaderException;


// Global vars
static bool VERBOSE = false;

static struct SwsContext* swsContext;

struct RgbData
{
    uint8_t* data = nullptr;
    size_t size = 0;
    int width = 0;
    int height = 0;
};

/**
 * Check if image has a grid configuration and return the grid id
 * @param reader
 * @param contextId
 * @return
 */
IdVector findGridItems(const HevcImageFileReader* reader, uint32_t contextId)
{
    IdVector gridItemIds;
    reader->getItemListByType(contextId, "grid", gridItemIds);

    if (gridItemIds.empty()) {
        throw logic_error("No grid items founds!");
    }

    return gridItemIds;
}

/**
 * Find thmb reference in metabox
 * @param reader
 * @param contextId
 * @param itemId
 * @return
 */
uint32_t findThumbnailId(const HevcImageFileReader* reader, uint32_t contextId, uint32_t itemId)
{
    IdVector thmbIds;
    reader->getReferencedToItemListByType(contextId, itemId, "thmb", thmbIds);

    if (thmbIds.empty()) {
        throw logic_error("Thumbnail ID not found!");
    }

    return thmbIds.at(0);
}

/**
 * Convert colorspace of decoded frame load into buffer
 * @param frame
 * @param dst
 * @param dst_size
 * @return the number of bytes written to dst, or a negative value on error
 */
int copyFrameInto(AVFrame* frame, uint8_t* dst, size_t dst_size)
{
    AVFrame* imgFrame = av_frame_alloc();
    int width = frame->width;
    int height = frame->height;

    uint8_t *tempBuffer = (uint8_t*) av_malloc(dst_size);

    struct SwsContext *sws_ctx = sws_getCachedContext(swsContext,
                                                      width, height, AV_PIX_FMT_YUV420P,
                                                      width, height, AV_PIX_FMT_RGB24,
                                                      0, nullptr, nullptr, nullptr);

    av_image_fill_arrays(imgFrame->data, imgFrame->linesize, tempBuffer, AV_PIX_FMT_RGB24, width, height, 1);
    uint8_t const* const* frameDataPtr = (uint8_t const* const*)frame->data;

    // Convert YUV to RGB
    sws_scale(sws_ctx, frameDataPtr, frame->linesize, 0, height, imgFrame->data, imgFrame->linesize);

    // Move RGB data in pixel order into memory
    const uint8_t* const* dataPtr = static_cast<const uint8_t* const*>(imgFrame->data);
    int size = static_cast<int>(dst_size);
    int ret = av_image_copy_to_buffer(dst, size, dataPtr, imgFrame->linesize, AV_PIX_FMT_RGB24, width, height, 1);

    av_free(imgFrame);
    av_free(tempBuffer);

    return ret;
}

/**
 * Get libav HEVC decoder
 * @return
 */
AVCodecContext* getHEVCDecoderContext()
{
    AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    AVCodecContext *c = avcodec_alloc_context3(codec);

    if (!c) {
        throw logic_error("Could not allocate video codec context");
    }

    if (avcodec_open2(c, c->codec, nullptr) < 0) {
        throw logic_error("Could not open codec");
    }

    return c;
}

/**
 * Decode HEVC frame and return loadable RGB data
 * @param hevcData
 * @param rgbData
 */
RgbData decodeFrame(DataVector hevcData)
{
    AVCodecContext* c = getHEVCDecoderContext();
    AVFrame* frame = av_frame_alloc();

    AVPacket* packet = av_packet_alloc();
    packet->size = static_cast<int>(hevcData.size());
    packet->data = &hevcData[0];

    char* errorDescription = new char[256];

    int sent = avcodec_send_packet(c, packet);
    if (sent < 0)
    {
        av_strerror(sent, errorDescription, 256);
        cerr << "Error sending packet to HEVC decoder: " << errorDescription << endl;
        throw sent;
    }

    int success = avcodec_receive_frame(c, frame);
    if (success != 0) {
        av_strerror(success, errorDescription, 256);
        cerr << "Error decoding frame: " << errorDescription << endl;
        throw success;
    }

    size_t bufferSize = static_cast<size_t>(av_image_get_buffer_size(AV_PIX_FMT_RGB24, frame->width, frame->height, 1));

    RgbData result = {};
    result.data = (uint8_t *) malloc(bufferSize);
    result.size = bufferSize;
    result.width = frame->width;
    result.height = frame->height;

    copyFrameInto(frame, result.data, result.size);

    avcodec_close(c);
    av_free(c);
    av_free(frame);

    return result;
}

/**
 * Extract EXIF data from HEIF
 * @param reader
 * @param contextId
 * @param itemId
 * @return
 */
DataVector extractExifData(HevcImageFileReader* reader, uint32_t contextId, uint32_t itemId)
{
    IdVector exifItemIds;
    DataVector exifData;

    reader->getReferencedToItemListByType(contextId, itemId, "cdsc", exifItemIds);

    if (exifItemIds.empty()) {
        throw logic_error("Exif Data ID (cdsc) not found!");
    }

    reader->getItemData(contextId, exifItemIds.at(0), exifData);

    if (exifData.empty()) {
        throw logic_error("Exif data is empty");
    }

    return exifData;
}

/**
 * libexif data loader without setting default values, blatantly stolen from vips
 * @param data
 * @param length
 * @return
 */
ExifData* exifLoadDataWithoutFix(const uint8_t *data, const uint32_t length)
{
    ExifData *ed;

    if(!(ed = exif_data_new())) {
        throw logic_error("could not allocate exif data");
    }

    exif_data_unset_option(ed, EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);
    exif_data_load_data(ed, data, length);

    return ed;
}

/**
 * Parse exif data and set meta fields to vips image
 * @param dataPtr
 * @param dataLength
 * @param image
 */
void parseAndAppendExif(uint8_t *dataPtr, uint32_t dataLength, VImage *image)
{
    ExifData* ed = exifLoadDataWithoutFix(dataPtr, dataLength);

    if (!ed) {
        throw logic_error("Failed to parse exif data");
    }

    // This doesn't to anything, we probably need to serialize all fields the same way vips does internally
    image->set(VIPS_META_EXIF_NAME, nullptr, ed->data, ed->size);

    // This does work though
    ExifByteOrder byteOrder = exif_data_get_byte_order(ed);
    ExifEntry *exifEntry = exif_data_get_entry(ed, EXIF_TAG_ORIENTATION);

    if (exifEntry) {;
        int orientation = exif_get_short(exifEntry->data, byteOrder);
        image->set(VIPS_META_ORIENTATION, orientation);
    }
}


/**
 * Get thumbnail from HEIC image
 * @param reader
 * @param contextId
 * @param gridItemId
 * @return
 */
VImage getThumbnailImage(HevcImageFileReader& reader, uint32_t contextId, uint32_t gridItemId)
{
    // Find Thumbnail ID
    const uint32_t thmbId = findThumbnailId(&reader, contextId, gridItemId);

    // Get thumbnail HEVC data
    DataVector hevcData;
    reader.getItemDataWithDecoderParameters(contextId, thmbId, hevcData);

    RgbData rgb = decodeFrame(hevcData);

    // Load image into vips and save as JPEG
    VImage thumbImg = VImage::new_from_memory(rgb.data, rgb.size, rgb.width, rgb.height, 3, VIPS_FORMAT_UCHAR);

    return thumbImg;
}

/**
 * Build image from HEIC grid item
 * @param reader
 * @param contextId
 * @param gridItemId
 * @return
 */
VImage getImage(HevcImageFileReader& reader, uint32_t contextId, uint32_t gridItemId, bool parallel = false)
{
    GridItem gridItem;
    gridItem = reader.getItemGrid(contextId, gridItemId);

    // Convenience vars
    uint32_t width = gridItem.outputWidth;
    uint32_t height = gridItem.outputHeight;
    uint32_t columns = gridItem.columnsMinusOne + 1;
    uint32_t rows = gridItem.rowsMinusOne + 1;

    if (VERBOSE) {
        cout << "Grid is " << width << "x" << height << " pixels in tiles " << columns << "x" << rows << endl;
    }

    // Find master tiles to extract
    IdVector tileItemIds;
    reader.getItemListByType(contextId, "master", tileItemIds);

    uint32_t firstTileId = tileItemIds.at(0);

    // Extract and decode all tiles

    vector<VImage> tiles;
    vector<future<RgbData>> decoderResults;

    for (uint32_t tileItemId : tileItemIds) {
        DataVector hevcData;
        reader.getItemDataWithDecoderParameters(contextId, tileItemId, firstTileId, hevcData);

        if (parallel) {
            decoderResults.push_back(async(decodeFrame, hevcData));
        } else {
            RgbData rgb = decodeFrame(hevcData);

            VImage img = VImage::new_from_memory(rgb.data, rgb.size, rgb.width, rgb.height, 3, VIPS_FORMAT_UCHAR);

            tiles.push_back(img);
        }
    }

    if (parallel) {
        for (future<RgbData> &futureData: decoderResults) {
            RgbData rgb = futureData.get();

            VImage img = VImage::new_from_memory(rgb.data, rgb.size, rgb.width, rgb.height, 3, VIPS_FORMAT_UCHAR);

            tiles.push_back(img);
        }
    }

    // Stitch tiles together

    VImage image = VImage::new_memory();

    image = image.arrayjoin(tiles, VImage::option()->set("across", (int)columns));

    image = image.extract_area(0, 0, width, height);

    return image;
}

/**
 * Rotate image according to Exif orientation when necessary
 * @param in
 * @return
 */
VImage rotateImage(VImage& in)
{
    int orientation = in.get_int(VIPS_META_ORIENTATION);

    switch (orientation) {
        case 2:
            return in.flip(VIPS_DIRECTION_HORIZONTAL);
        case 3:
            return in.rot180();
        case 4:
            return in.flip(VIPS_DIRECTION_VERTICAL);
        case 5:
            return in.rot90().flip(VIPS_DIRECTION_HORIZONTAL);
        case 6:
            return in.rot90();
        case 7:
            return in.rot270().flip(VIPS_DIRECTION_HORIZONTAL);
        case 8:
            return in.rot270();
        default:
            return in;
    }
}

/**
 * Save created image to file
 * @param img
 * @param fileName
 * @param options
 */
void saveImage(VImage& img, const string& fileName, cxxopts::Options& options)
{
    chrono::steady_clock::time_point begin_buildImage = chrono::steady_clock::now();

    char * outName = const_cast<char *>(fileName.c_str());

    string ext = fileName.substr(fileName.find_last_of(".") + 1);

    // Supported image output formats
    set<string> jpgExt = {"jpg", "jpeg", "JPG", "JPEG"};
    set<string> pngExt = {"png", "PNG"};
    set<string> tiffExt = {"tiff", "TIFF"};
    set<string> ppmExt = {"ppm", "PPM"};

    if (jpgExt.find(ext) != jpgExt.end()) {
        int quality = options["quality"].as<int>();
        img.jpegsave(outName, VImage::option()->set("Q", quality));
    } else if (tiffExt.find(ext) != tiffExt.end()) {
        img = rotateImage(img);
        img.set(VIPS_META_ORIENTATION, 1);
        img.tiffsave(outName);
    } else if (pngExt.find(ext) != pngExt.end()) {
        rotateImage(img).pngsave(outName);
    } else if (ppmExt.find(ext) != ppmExt.end()) {
        rotateImage(img).ppmsave(outName);
    } else {
        throw logic_error("Unknown image extension: " + ext);
    }

    chrono::steady_clock::time_point end_buildImage = chrono::steady_clock::now();
    long buildImageTime = chrono::duration_cast<chrono::milliseconds>(end_buildImage - begin_buildImage).count();

    if (VERBOSE) {
        cout << "Saving image: " << buildImageTime << "ms" << endl;
    }
}

/**
 * Main entry point
 * @param inputFilename
 * @param outputFilename
 * @param options
 * @return
 */
int convert(const string& inputFilename, const string& outputFilename, cxxopts::Options& options)
{
    HevcImageFileReader reader;
    reader.initialize(inputFilename);
    const uint32_t contextId = reader.getFileProperties().rootLevelMetaBoxProperties.contextId;

    // Detect grid
    const IdVector& gridItems = findGridItems(&reader, contextId);

    uint32_t gridItemId = gridItems.at(0);

    VIPS_INIT("tifig");
    avcodec_register_all();

    chrono::steady_clock::time_point begin_encode = chrono::steady_clock::now();

    VImage image;

    bool useThumbnail = options["thumbnail"].as<bool>();
    bool decodeParallel = options["parallel"].as<bool>();

    if (useThumbnail) {
        image = getThumbnailImage(reader, contextId, gridItemId);
    } else {
        image = getImage(reader, contextId, gridItemId, decodeParallel);
    }

    chrono::steady_clock::time_point end_encode = chrono::steady_clock::now();
    long tileEncodeTime = chrono::duration_cast<chrono::milliseconds>(end_encode - begin_encode).count();

    if (VERBOSE) {
        cout << "Export & decode HEVC: " << tileEncodeTime << "ms" << endl;
    }

    DataVector exifData =  extractExifData(&reader, contextId, gridItemId);

    uint8_t* exifDataPtr = &exifData[4];
    uint32_t exifDataLength = static_cast<uint32_t>(exifData.size() - 4);

    parseAndAppendExif(exifDataPtr, exifDataLength, &image);

    saveImage(image, outputFilename, options);

    vips_shutdown();

    return 0;
}

int main(int argc, char* argv[])
{
    // Disable colr and pixi boxes unknown warnings from libheif
    Log::getWarningInstance().setLevel(Log::LogLevel::ERROR);

    int retval = -1;

    try {

        cxxopts::Options options(argv[0], "Converts iOS 11 HEIC images to practical formats");

        options.positional_help("input_file output_file");

        options.parse_positional(vector<string>{"input", "output"});

        options.add_options()
                ("i,input", "Input HEIF image", cxxopts::value<string>())
                ("o,output", "Output image path", cxxopts::value<string>())
                ("q,quality", "Output JPEG quality", cxxopts::value<int>()
                        ->default_value("90")->implicit_value("90"))
                ("v,verbose", "Verbose output", cxxopts::value<bool>(VERBOSE))
                ("p,parallel", "Decode tiles in parallel", cxxopts::value<bool>())
                ("t,thumbnail", "Export thumbnail", cxxopts::value<bool>())
                ;

        options.parse(argc, argv);

        if (options.count("input") && options.count("output")) {
            string inputFileName = options["input"].as<string>();
            string outputFileName = options["output"].as<string>();

            chrono::steady_clock::time_point begin = chrono::steady_clock::now();

            retval = convert(inputFileName, outputFileName, options);

            chrono::steady_clock::time_point end = chrono::steady_clock::now();
            long duration = chrono::duration_cast<chrono::milliseconds>(end - begin).count();

            if (VERBOSE) {
                cout << "Total Time: " << duration << "ms" << endl;
            }
        } else {
            cout << options.help() << endl;
        }


    }
    catch (const cxxopts::OptionException& oe) {
        cout << "error parsing options: " << oe.what() << endl;
    }
    catch (const FileReaderException& fre) {
        cerr << "Could not read HEIF image: " << fre.what() << endl;
    }
    catch (const logic_error& le) {
        cerr << le.what() << endl;
    }
    catch (...) {
        cerr << "Conversion failed" << endl;
    }

    return retval;
}

