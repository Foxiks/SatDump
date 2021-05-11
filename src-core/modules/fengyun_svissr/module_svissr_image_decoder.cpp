#include "module_svissr_image_decoder.h"
#include "logger.h"
#include "modules/common/differential/nrzs.h"
#include "imgui/imgui.h"
#include "imgui/imgui_image.h"
#include <filesystem>

#define FRAME_SIZE 44356

// Return filesize
size_t getFilesize(std::string filepath);

namespace fengyun_svissr
{
    std::string SVISSRImageDecoderModule::getSvissrFilename(std::tm *timeReadable, int channel)
    {
        std::string utc_filename = sat_name + "_" + std::to_string(channel) + "_" +                                                                             // Satellite name and channel
                                   std::to_string(timeReadable->tm_year + 1900) +                                                                               // Year yyyy
                                   (timeReadable->tm_mon + 1 > 9 ? std::to_string(timeReadable->tm_mon + 1) : "0" + std::to_string(timeReadable->tm_mon + 1)) + // Month MM
                                   (timeReadable->tm_mday > 9 ? std::to_string(timeReadable->tm_mday) : "0" + std::to_string(timeReadable->tm_mday)) + "T" +    // Day dd
                                   (timeReadable->tm_hour > 9 ? std::to_string(timeReadable->tm_hour) : "0" + std::to_string(timeReadable->tm_hour)) +          // Hour HH
                                   (timeReadable->tm_min > 9 ? std::to_string(timeReadable->tm_min) : "0" + std::to_string(timeReadable->tm_min)) +             // Minutes mm
                                   (timeReadable->tm_sec > 9 ? std::to_string(timeReadable->tm_sec) : "0" + std::to_string(timeReadable->tm_sec)) + "Z";        // Seconds ss
        return utc_filename;
    }

    void SVISSRImageDecoderModule::writeImages(std::string directory)
    {
        const time_t timevalue = time(0);
        std::tm *timeReadable = gmtime(&timevalue);
        std::string timestamp = std::to_string(timeReadable->tm_year + 1900) + "-" +
                                (timeReadable->tm_mon + 1 > 9 ? std::to_string(timeReadable->tm_mon + 1) : "0" + std::to_string(timeReadable->tm_mon + 1)) + "-" +
                                (timeReadable->tm_mday > 9 ? std::to_string(timeReadable->tm_mday) : "0" + std::to_string(timeReadable->tm_mday)) + "_" +
                                (timeReadable->tm_hour > 9 ? std::to_string(timeReadable->tm_hour) : "0" + std::to_string(timeReadable->tm_hour)) + "-" +
                                (timeReadable->tm_min > 9 ? std::to_string(timeReadable->tm_min) : "0" + std::to_string(timeReadable->tm_min));

        std::this_thread::sleep_for(std::chrono::seconds(5)); // Wait a bit
        logger->info("Full disk finished, saving at S-VISSR_" + timestamp + "...");

        std::filesystem::create_directory(directory + "/S-VISSR_" + timestamp);

        std::string disk_folder = directory + "/S-VISSR_" + timestamp;

        logger->info("Channel 1... " + getSvissrFilename(timeReadable, 1) + ".png");
        image5.save_png(std::string(disk_folder + "/" + getSvissrFilename(timeReadable, 1) + ".png").c_str());

        logger->info("Channel 2... " + getSvissrFilename(timeReadable, 2) + ".png");
        image1.save_png(std::string(disk_folder + "/" + getSvissrFilename(timeReadable, 2) + ".png").c_str());

        logger->info("Channel 3... " + getSvissrFilename(timeReadable, 3) + ".png");
        image2.save_png(std::string(disk_folder + "/" + getSvissrFilename(timeReadable, 3) + ".png").c_str());

        logger->info("Channel 4... " + getSvissrFilename(timeReadable, 4) + ".png");
        image3.save_png(std::string(disk_folder + "/" + getSvissrFilename(timeReadable, 4) + ".png").c_str());

        logger->info("Channel 5... " + getSvissrFilename(timeReadable, 5) + ".png");
        image4.save_png(std::string(disk_folder + "/" + getSvissrFilename(timeReadable, 5) + ".png").c_str());

        writingImage = false;
    }

    SVISSRImageDecoderModule::SVISSRImageDecoderModule(std::string input_file, std::string output_file_hint, std::map<std::string, std::string> parameters) : ProcessingModule(input_file, output_file_hint, parameters),
                                                                                                                                                              sat_name(parameters["satname"])
    {
        frame = new uint8_t[FRAME_SIZE];

        // Counters and so
        writingImage = false;
        endCount = 0;
        nonEndCount = 0;
        lastNonZero = 0;
        backwardScan = false;

        vissrImageReader.reset();

        // Init thread pool
        imageSavingThreadPool = std::make_shared<ctpl::thread_pool>(1);
    }

    std::vector<ModuleDataType> SVISSRImageDecoderModule::getInputTypes()
    {
        return {DATA_FILE, DATA_STREAM};
    }

    std::vector<ModuleDataType> SVISSRImageDecoderModule::getOutputTypes()
    {
        return {DATA_FILE};
    }

    SVISSRImageDecoderModule::~SVISSRImageDecoderModule()
    {
        delete[] frame;

        if (textureID != 0)
        {
            delete[] textureBuffer;
            deleteImageTexture(textureID);
        }
    }

    void SVISSRImageDecoderModule::process()
    {
        if (input_data_type == DATA_FILE)
            filesize = getFilesize(d_input_file);
        else
            filesize = 0;
        if (input_data_type == DATA_FILE)
            data_in = std::ifstream(d_input_file, std::ios::binary);

        std::string directory = d_output_file_hint.substr(0, d_output_file_hint.rfind('/')) + "/IMAGE";

        if (!std::filesystem::exists(directory))
            std::filesystem::create_directory(directory);

        logger->info("Using input frames " + d_input_file);
        logger->info("Decoding to " + directory);

        time_t lastTime = 0;

        while (input_data_type == DATA_FILE ? !data_in.eof() : input_active.load())
        {
            // Read a buffer
            if (input_data_type == DATA_FILE)
                data_in.read((char *)frame, FRAME_SIZE);
            else
                input_fifo->read((uint8_t *)frame, FRAME_SIZE);

            // Do the actual work
            {
                // Parse counter
                int counter = frame[67] << 8 | frame[68];

                // Safeguard
                if (counter > 2500)
                    continue;

                // Parse scan status
                int status = frame[3] % (int)pow(2, 2); // Decoder scan status

                // We only want forward scan data
                if (status != 3)
                {
                    backwardScan = true;
                    continue;
                }

                backwardScan = false;

                //std::cout << counter << std::endl;

                // Try to detect a new scan
                // This is not the best way, but it works...
                if (counter > 2490 && counter <= 2500)
                {
                    endCount++;
                    nonEndCount = 0;

                    if (endCount > 5)
                    {
                        endCount = 0;
                        logger->info("Full disk end detected!");

                        if (!writingImage)
                        {
                            writingImage = true;

                            // Backup images
                            image1 = vissrImageReader.getImageIR1();
                            image2 = vissrImageReader.getImageIR2();
                            image3 = vissrImageReader.getImageIR3();
                            image4 = vissrImageReader.getImageIR4();
                            image5 = vissrImageReader.getImageVIS();

                            // Write those
                            imageSavingThreadPool->push([&](int) { writeImages(directory); });

                            // Reset readers
                            vissrImageReader.reset();
                        }
                    }
                }
                else
                {
                    nonEndCount++;
                    if (endCount > 0)
                        endCount -= 1;
                }

                // Process it
                vissrImageReader.pushFrame(frame);

                approx_progess = round(((float)counter / 2500.0f) * 1000.0f) / 10.0f;
            }

            if (input_data_type == DATA_FILE)
                progress = data_in.tellg();

            if (time(NULL) % 10 == 0 && lastTime != time(NULL))
            {
                lastTime = time(NULL);
                logger->info("Progress " + std::to_string(round(((float)progress / (float)filesize) * 1000.0f) / 10.0f) + "%");
            }
        }

        if (input_data_type == DATA_FILE)
            data_in.close();

        logger->info("Wait for in-progress images...");
        while (writingImage)
            std::this_thread::sleep_for(std::chrono::seconds(1));

        logger->info("Dump remaining data...");
        {
            writingImage = true;

            // Backup images
            image1 = vissrImageReader.getImageIR1();
            image2 = vissrImageReader.getImageIR2();
            image3 = vissrImageReader.getImageIR3();
            image4 = vissrImageReader.getImageIR4();
            image5 = vissrImageReader.getImageVIS();

            // Write those
            imageSavingThreadPool->push([directory, this](int) {
                writeImages(directory);
            });
        }

        while (writingImage)
            std::this_thread::sleep_for(std::chrono::seconds(1));

        imageSavingThreadPool->stop();
    }

    const ImColor colorNosync = ImColor::HSV(0 / 360.0, 1, 1, 1.0);
    const ImColor colorSyncing = ImColor::HSV(39.0 / 360.0, 0.93, 1, 1.0);
    const ImColor colorSynced = ImColor::HSV(113.0 / 360.0, 1, 1, 1.0);

    void SVISSRImageDecoderModule::drawUI(bool window)
    {
        if (textureID == 0)
        {
            textureID = makeImageTexture();
            textureBuffer = new uint32_t[2501 * 2291];
        }

        ImGui::Begin("S-VISSR Image Decoder", NULL, window ? NULL : NOWINDOW_FLAGS);

        // This is outer crap...
        ImGui::BeginGroup();
        {
            ushort_to_rgba(vissrImageReader.imageBufferIR1, textureBuffer, 2501 * 2291);
            updateImageTexture(textureID, textureBuffer, 2291, 2501);
            ImGui::Image((void *)(intptr_t)textureID, {200 * ui_scale, 200 * ui_scale});
        }
        ImGui::EndGroup();

        ImGui::SameLine();

        ImGui::BeginGroup();
        {
            ImGui::Button("Full Disk Progress", {200 * ui_scale, 20 * ui_scale});
            ImGui::ProgressBar((float)approx_progess / 100.0f, ImVec2(200 * ui_scale, 20 * ui_scale));
            ImGui::Text("State : ");
            ImGui::SameLine();
            if (backwardScan)
            {
                ImGui::TextColored(colorNosync, "Imager rollback...");
            }
            else
            {
                if (writingImage)
                    ImGui::TextColored(colorSynced, "Writing images...");
                else
                    ImGui::TextColored(colorSyncing, "Receiving...");
            }
        }
        ImGui::EndGroup();

        ImGui::ProgressBar((float)progress / (float)filesize, ImVec2(ImGui::GetWindowWidth() - 10, 20 * ui_scale));

        ImGui::End();
    }

    std::string SVISSRImageDecoderModule::getID()
    {
        return "fengyun_svissr_image_decoder";
    }

    std::vector<std::string> SVISSRImageDecoderModule::getParameters()
    {
        return {"satname"};
    }

    std::shared_ptr<ProcessingModule> SVISSRImageDecoderModule::getInstance(std::string input_file, std::string output_file_hint, std::map<std::string, std::string> parameters)
    {
        return std::make_shared<SVISSRImageDecoderModule>(input_file, output_file_hint, parameters);
    }
} // namespace elektro