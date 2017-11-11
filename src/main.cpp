#include <chrono>
#include <cxxopts.hpp>
#include <exif.h>
#include <hevcimagefilereader.hpp>
#include <log.hpp>
#include <vips/vips8>

extern "C" {
    #include <libavutil/opt.h>
    #include <libavutil/imgutils.h>
    #include <libavcodec/avcodec.h>
    #include <libswscale/swscale.h>
};

using namespace std;
using namespace vips;

typedef ImageFileReaderInterface::DataVector DataVector;
typedef ImageFileReaderInterface::IdVector IdVector;
typedef ImageFileReaderInterface::GridItem GridItem;
typedef ImageFileReaderInterface::FileReaderException FileReaderException;


// Global vars
static bool VERBOSE = false;
static int QUALITY = 90;

static struct SwsContext* swsContext;


/**
 * Check if image has a grid configuration and return the grid id
 * @param reader
 * @param contextId
 * @return
 */
IdVector findGridItems(const HevcImageFileReader *reader, uint32_t contextId)
{
    IdVector gridItemIds;
    reader->getItemListByType(contextId, "grid", gridItemIds);

    if (gridItemIds.size() == 0) {
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
uint32_t findThumbnailId(const HevcImageFileReader *reader, uint32_t contextId, uint32_t itemId)
{
    IdVector thmbIds;
    reader->getReferencedToItemListByType(contextId, itemId, "thmb", thmbIds);

    if (thmbIds.size() == 0) {
        throw logic_error("Thumbnail ID not found!");
    }

    return thmbIds.at(0);
}

/**
 * Convert colorspace of decoded frame to RGB and load vips image
 * @param frame
 * @return
 */
VImage loadImageFromDecodedFrame(AVFrame *frame)
{
    AVFrame* imgFrame = av_frame_alloc();
    int width = frame->width;
    int height = frame->height;

    int imgRGB24size = avpicture_get_size(PIX_FMT_RGB24, width, height);;
    uint8_t *tempBuffer = (uint8_t*) av_malloc(imgRGB24size);

    struct SwsContext *sws_ctx = sws_getCachedContext(swsContext,
                                                      width, height, AV_PIX_FMT_YUV420P,
                                                      width, height, AV_PIX_FMT_RGB24,
                                                      0, nullptr, nullptr, nullptr);

    av_image_fill_arrays(imgFrame->data, imgFrame->linesize, tempBuffer, PIX_FMT_RGB24, width, height, 1);
    auto const* const* frameDataPtr = (uint8_t const* const*)frame->data;

    // Convert YUV to RGB
    sws_scale(sws_ctx, frameDataPtr, frame->linesize, 0, height, imgFrame->data, imgFrame->linesize);

    // Move RGB data in pixel order into memory
    uint8_t* buff = (uint8_t*) malloc(imgRGB24size);
    const auto* const* dataPtr = (const uint8_t* const*)imgFrame->data;
    av_image_copy_to_buffer(buff, imgRGB24size, dataPtr, imgFrame->linesize, AV_PIX_FMT_RGB24, width, height, 1);

    av_free(tempBuffer);

    return VImage::new_from_memory(buff, imgRGB24size, width, height, 3, VIPS_FORMAT_UCHAR);
}

/**
 * Decode HEVC Frame using libav
 * @param hevcData
 * @return
 */
VImage decodeHEVCFrame(DataVector& hevcData)
{
    AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    AVCodecContext *c;

    c = avcodec_alloc_context3(codec);
    if (!c) {
        throw logic_error("Could not allocate video codec context");
    }

    if (avcodec_open2(c, codec, NULL) < 0) {
        throw logic_error("Could not open codec");
    }

    AVFrame *frame = av_frame_alloc();

    AVPacket avpkt;
    av_init_packet(&avpkt);
    avpkt.size = hevcData.size();
    avpkt.data = &hevcData[0];

    int success;
    int len = avcodec_decode_video2(c, frame, &success, &avpkt);
    if (len < 0 || !success) {
        throw logic_error("Failed to decode frame");
    }

    VImage image = loadImageFromDecodedFrame(frame);

    avcodec_close(c);
    av_free(c);
    av_free(frame);

    return image;
}


/**
 * Extract EXIF data from HEIF
 * @param reader
 * @param contextId
 * @param itemId
 * @return
 */
easyexif::EXIFInfo extractExifData(HevcImageFileReader *reader, uint32_t contextId, uint32_t itemId)
{
    IdVector exifItemIds;
    DataVector exifData;

    reader->getReferencedToItemListByType(contextId, itemId, "cdsc", exifItemIds);

    if (exifItemIds.size() == 0) {
        throw logic_error("Exif Data ID (cdsc) not found!");
    }

    reader->getItemData(contextId, exifItemIds.at(0), exifData);

    easyexif::EXIFInfo exifInfo;
    int parseRet = exifInfo.parseFromEXIFSegment(&exifData[4], exifData.size() - 4);

    if (parseRet != 0) {
        throw logic_error("Failed to parse EXIF data!");
    }

    return exifInfo;
}

/**
 * Build full image from tiles
 * @param width
 * @param height
 * @param columns
 * @param tiles
 */
VImage buildFullImage(int width, int height, int columns, vector<VImage> tiles)
{
    if (tiles.size() == 0) {
        throw logic_error("No tiles given to build image");
    }

    const int tileSize = tiles.at(0).width();

    VImage combined = VImage::new_matrix(width, height);

    int offsetX = 0;
    int offsetY = 0;

    for (int i = 0; i < tiles.size(); i++) {

        VImage in = tiles.at(i);

        combined = combined.insert(in, offsetX, offsetY);

        if ((i + 1) % columns == 0) {
            offsetY += tileSize;
            offsetX = 0;
        }
        else {
            offsetX += tileSize;
        }
    }

    return combined;
}


/**
 *
 * @param inputFilename
 * @param outputFilename
 * @return
 */
int exportThumbnail(string inputFilename, string outputFilename)
{
    HevcImageFileReader reader;
    reader.initialize(inputFilename);
    const uint32_t contextId = reader.getFileProperties().rootLevelMetaBoxProperties.contextId;

    // Detect grid
    const auto &gridItems = findGridItems(&reader, contextId);

    uint32_t gridItemId = gridItems.at(0);

    // Find Thumbnail ID
    const uint32_t thmbId = findThumbnailId(&reader, contextId, gridItemId);

    // Extract EXIF data;
    easyexif::EXIFInfo exifInfo = extractExifData(&reader, contextId, gridItemId);

    // Get thumbnail HEVC data
    DataVector hevcData;
    reader.getItemDataWithDecoderParameters(contextId, thmbId, hevcData);

    // Decode HEVC Frame
    VImage thumbImg = decodeHEVCFrame(hevcData);

    thumbImg.set(VIPS_META_ORIENTATION, exifInfo.Orientation);

    char * jpegName = const_cast<char *>(outputFilename.c_str());
    thumbImg.jpegsave(jpegName, VImage::option()->set("Q", QUALITY));

    return 0;
}


/**
 *
 * @param inputFilename
 * @param outputFilename
 * @return
 */
int convertToJpeg(string inputFilename, string outputFilename)
{
    HevcImageFileReader reader;
    reader.initialize(inputFilename);
    const uint32_t contextId = reader.getFileProperties().rootLevelMetaBoxProperties.contextId;

    // Detect grid
    const auto &gridItems = findGridItems(&reader, contextId);

    uint32_t gridItemId = gridItems.at(0);
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

    // Extract EXIF data;
    easyexif::EXIFInfo exifInfo = extractExifData(&reader, contextId, gridItemId);

    // Find master tiles to extract
    IdVector tileItemIds;
    reader.getItemListByType(contextId, "master", tileItemIds);

    uint32_t firstTileId = tileItemIds.at(0);

    // Extract and decode all tiles

    chrono::steady_clock::time_point begin_encode = chrono::steady_clock::now();

    vector<VImage> tiles;

    for (auto &tileItemId : tileItemIds) {

        DataVector hevcData;
        reader.getItemDataWithDecoderParameters(contextId, tileItemId, firstTileId, hevcData);

        VImage img = decodeHEVCFrame(hevcData);

        tiles.push_back(img);
    }

    chrono::steady_clock::time_point end_encode = chrono::steady_clock::now();
    long tileEncodeTime = chrono::duration_cast<chrono::milliseconds>(end_encode - begin_encode).count();

    if (VERBOSE) {
        cout << "Export & encode tiles " << tileEncodeTime << "ms" << endl;
    }

    // Stitch tiles together

    chrono::steady_clock::time_point begin_buildImage = chrono::steady_clock::now();

    VImage result = buildFullImage(width, height, columns, tiles);

    result.set(VIPS_META_ORIENTATION, exifInfo.Orientation);

    char * jpegName = const_cast<char *>(outputFilename.c_str());
    result.jpegsave(jpegName, VImage::option()->set("Q", QUALITY));

    chrono::steady_clock::time_point end_buildImage = chrono::steady_clock::now();
    long buildImageTime = chrono::duration_cast<chrono::milliseconds>(end_buildImage - begin_buildImage).count();

    if (VERBOSE) {
        cout << "Building image " << buildImageTime << "ms" << endl;
    }

    return 0;
}

int main(int argc, char* argv[])
{
    // Disable colr and pixi boxes unknown warnings from libheif
    Log::getWarningInstance().setLevel(Log::LogLevel::ERROR);

    int retval = -1;

    VIPS_INIT(argv[0]);

    avcodec_register_all();

    try {

        cxxopts::Options options(argv[0], "Convert HEIF images to JPEG");

        options.positional_help("input_file output_file");

        options.parse_positional(vector<string>{"input", "output"});

        options.add_options()
                ("i,input", "Input HEIF image", cxxopts::value<string>())
                ("o,output", "Output JPEG image", cxxopts::value<string>())
                ("q,quality", "Output JPEG quality (1-100) default 90", cxxopts::value<int>(QUALITY))
                ("v,verbose", "Verbose output", cxxopts::value<bool>(VERBOSE))
                ("t,thumbnail", "Export thumbnail", cxxopts::value<bool>())
                ;

        options.parse(argc, argv);

        if (options.count("input") && options.count("output")) {
            string inputFileName = options["input"].as<string>();
            string outputFileName = options["output"].as<string>();
            bool thumb = options["thumbnail"].as<bool>();

            chrono::steady_clock::time_point begin = chrono::steady_clock::now();

            if (thumb) {
                retval = exportThumbnail(inputFileName, outputFileName);
            } else {
                retval = convertToJpeg(inputFileName, outputFileName);
            }

            chrono::steady_clock::time_point end = chrono::steady_clock::now();
            int duration = chrono::duration_cast<chrono::milliseconds>(end - begin).count();

            if (VERBOSE) {
                cout << "Total Time " << duration << "ms" << endl;
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

    vips_shutdown();

    return retval;
}

