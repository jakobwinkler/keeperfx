/******************************************************************************/
// Best 8bpp palette selector for a series of PNG files
/******************************************************************************/
/** @file png2bestpal.cpp
 *     Program code file.
 * @par Purpose:
 *     Contains code to read PNG files and select the best special 8bpp
 *     palette for KeeperFX which would allow to display all the files.
 *     Based on png2ico project.
 * @par Comment:
 *     None.
 * @author   Tomasz Lis <listom@gmail.com>
 * @author   Matthias S. Benkmann <matthias@winterdrache.de>
 * @date     25 Jul 2012 - 18 Aug 2012
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/

#include <cstdio>
#include <vector>
#include <climits>
#include <getopt.h>

#if __GNUC__ > 2
#include <ext/hash_map>
#else
#include <hash_map>
#endif

#include <png.h>

#include "png2bestpal_version.h"

using namespace std;
namespace __gnu_cxx{};
using namespace __gnu_cxx;

enum {
    ERR_OK          =  0,
    ERR_CANT_OPEN   = -1, // fopen problem
    ERR_BAD_FILE    = -2, // incorrect file format
    ERR_NO_MEMORY   = -3, // malloc error
    ERR_FILE_READ   = -4, // fget/fread/fseek error
    ERR_FILE_WRITE  = -5, // fput/fwrite error
    ERR_LIMIT_EXCEED= -6, // static limit exceeded
};

#define LogMsg(format,args...) fprintf(stdout,format "\n", ## args)
#define LogErr(format,args...) fprintf(stderr,format "\n", ## args)

int verbose = 0;

/** A struct closing non-global command line parameters */
class ProgramOptions {
public:
    ProgramOptions()
    {
        clear();
    }
    void clear()
    {
        fnames_inp.clear();
        fname_pal = "";
    }
    std::vector<std::string> fnames_inp;
    std::string fname_pal;
};

//maximum quadratic euclidean distance in RGB color space that a palette color may have to a source color assigned to it before a warning is issued
const int color_reduce_warning_threshold=512;

class ImageData
{
public:
    ImageData():png_ptr(NULL),info_ptr(NULL),end_info(NULL),width(0),height(0),
               transMap(NULL),transparency_threshold(196){}
    png_structp png_ptr;
    png_infop info_ptr;
    png_infop end_info;
    png_uint_32 width, height;
    png_bytepp transMap;
    int color_type;
    int transparency_threshold;
};

typedef hash_map<unsigned long,signed int> MapQuadToPal;
typedef std::vector<png_color> ColorPalette;

class WorkingSet
{
public:
    WorkingSet():requested_colors(0),col_bits(0){}
    void requestedColors(int reqColors)
    {
        requested_colors = reqColors;
        for (col_bits=1; (1<<col_bits) < requested_colors; col_bits++);
    }
    int requestedColors(void)
    { return requested_colors; }
    int requestedColorBPP(void)
    { return col_bits; }
    void addPaletteQuad(unsigned long quad)
    {
        int palentry = palette.size();
        png_color ncol;
        ncol.red = quad&255;
        ncol.green = (quad>>8)&255;
        ncol.blue = (quad>>16)&255;
        palette.push_back(ncol);
        mapQuadToPalEntry[quad] = palentry;
    }
    std::vector<ImageData> images;
    ColorPalette palette;
    MapQuadToPal mapQuadToPalEntry;
private:
    int requested_colors;
    int col_bits;
};

typedef bool (*checkTransparent_t)(png_bytep, ImageData&);

bool checkTransparent1(png_bytep data, ImageData& img)
{
  return (data[3]<img.transparency_threshold);
}

bool checkTransparent3(png_bytep, ImageData&)
{
  return false;
}

//returns true if color reduction resulted in at least one of the image's colors
//being mapped to a palette color with a quadratic distance of more than
//color_reduce_warning_threshold
bool convert_rgb_to_indexed(WorkingSet& ws, ImageData& img, bool hasAlpha)
{
    int maxColors = ws.requestedColors();

    checkTransparent_t checkTrans=checkTransparent1;
    int bytesPerPixel=4;
    if (!hasAlpha)
    {
        bytesPerPixel=3;
        checkTrans=checkTransparent3;
    }

    //first pass: gather all colors, make sure
    //alpha channel (if present) contains only 0 and 255
    //if an alpha channel is present, set all transparent pixels to RGBA (0,0,0,0)
    //transparent pixels will already be mapped to palette entry 0, non-transparent
    //pixels will not get a mapping yet (-1)
    png_bytep* row_pointers=png_get_rows(img.png_ptr, img.info_ptr);

    for (int y=img.height-1; y>=0; --y)
    {
        png_bytep pixel=row_pointers[y];
        for (unsigned i=0; i<img.width; ++i)
        {
            unsigned int quad=pixel[0]+(pixel[1]<<8)+(pixel[2]<<16);
            bool trans=(*checkTrans)(pixel,img);

            if (hasAlpha)
            {
                if (trans) {
                    pixel[0]=0;
                    pixel[1]=0;
                    pixel[2]=0;
                    pixel[3]=0;
                    quad=0;
                } else {
                    pixel[3]=255;
                }

                quad+=(pixel[3]<<24);
            }
            else if (!trans) {
                quad+=(255<<24);
            }

            if (trans)
                ws.mapQuadToPalEntry[quad]=0;
            else
                ws.mapQuadToPalEntry[quad]=-1;

            pixel+=bytesPerPixel;
        }
    }

    //always allocate entry 0 to black and entry 1 to white because
    //sometimes AND mask is interpreted as color index

    //map (non-transparent) black to entry 0
    ws.addPaletteQuad(255<<24);
    //map (non-transparent) white to entry 1
    ws.addPaletteQuad(255|(255<<8)|(255<<16)|(255<<24));

    //Now fill up the palette with colors from the image by repeatedly picking the
    //color most different from the previously picked colors and adding this to the
    //palette. This is done to make sure that in case there are more image colors than
    //palette entries, palette entries are not wasted on similar colors.
    while(ws.palette.size() < maxColors)
    {
        unsigned int mostDifferentQuad=0;
        int mdqMinDist=-1; //smallest distance to an entry in the palette for mostDifferentQuad
        int mdqDistSum=-1; //sum over all distances to palette entries for mostDifferentQuad
        MapQuadToPal::iterator stop = ws.mapQuadToPalEntry.end();
        MapQuadToPal::iterator iter = ws.mapQuadToPalEntry.begin();
        while(iter!=stop)
        {
            MapQuadToPal::value_type& mapping=*iter++;
            if (mapping.second<0)
            {
                unsigned int quad=mapping.first;
                int red=quad&255;  //must be signed
                int green=(quad>>8)&255;
                int blue=(quad>>16)&255;
                int distSum=0;
                int minDist=INT_MAX;
                ColorPalette::iterator paliter;
                for (paliter = ws.palette.begin(); paliter != ws.palette.end(); paliter++)
                {
                    int dist=(red - paliter->red);
                    dist*=dist;
                    int temp=(green - paliter->green);
                    dist+=temp*temp;
                    temp=(blue - paliter->blue);
                    dist+=temp*temp;
                    if (dist<minDist) minDist=dist;
                    distSum += dist;
                }

                if (minDist>mdqMinDist || (minDist==mdqMinDist && distSum>mdqDistSum))
                {
                    mostDifferentQuad=quad;
                    mdqMinDist=minDist;
                    mdqDistSum=distSum;
                }
            }
        }

        if (mdqMinDist>0) {
            /* If we have found a most different quad, add it to the palette,
             * and map it to the new palette entry.
             */
            ws.addPaletteQuad(mostDifferentQuad);
        } else {
            /* otherwise (i.e. all quads are mapped) the palette is finished
             */
            break;
        }
    }

    //Now map all yet unmapped colors to the most appropriate palette entry
    MapQuadToPal::iterator stop = ws.mapQuadToPalEntry.end();
    MapQuadToPal::iterator iter = ws.mapQuadToPalEntry.begin();
    while(iter!=stop)
    {
        MapQuadToPal::value_type& mapping=*iter++;
        if (mapping.second<0)
        {
            unsigned int quad=mapping.first;
            int red=quad&255;  //must be signed
            int green=(quad>>8)&255;
            int blue=(quad>>16)&255;
            int minDist=INT_MAX;
            int bestIndex=0;
            ColorPalette::iterator paliter;
            for (paliter = ws.palette.begin(); paliter != ws.palette.end(); paliter++)
            {
                int dist=(red - paliter->red);
                dist*=dist;
                int temp=(green - paliter->green);
                dist+=temp*temp;
                temp=(blue - paliter->blue);
                dist+=temp*temp;
                if (dist<minDist) {
                    minDist=dist;
                    bestIndex = (paliter - ws.palette.begin());
                }
            }

            mapping.second=bestIndex;
        }
    }

    return true;
}

/** Adjusts all palette entries to be the mean of all colors mapped to it.
 */
bool update_palette_colors_to_average_of_uses(WorkingSet& ws)
{
    ColorPalette::iterator paliter;
    for (paliter = ws.palette.begin(); paliter != ws.palette.end(); paliter++)
    {
        int red=0;
        int green=0;
        int blue=0;
        int numMappings=0;
        MapQuadToPal::iterator stop = ws.mapQuadToPalEntry.end();
        MapQuadToPal::iterator iter = ws.mapQuadToPalEntry.begin();
        while(iter!=stop)
        {
            MapQuadToPal::value_type& mapping=*iter++;
            if (mapping.second == (paliter - ws.palette.begin()))
            {
                unsigned int quad=mapping.first;
                red+=quad&255;
                green+=(quad>>8)&255;
                blue+=(quad>>16)&255;
                ++numMappings;
            }
        }

        if (numMappings>0)
        {
            paliter->red=(red+red+numMappings)/(numMappings+numMappings);
            paliter->green=(green+green+numMappings)/(numMappings+numMappings);
            paliter->blue=(blue+blue+numMappings)/(numMappings+numMappings);
        }
    }

    return true;
}

char *file_name_change_extension(const char *fname_inp,const char *ext)
{
    char *fname;
    char *tmp1,*tmp2;
    if (fname_inp == NULL)
      return NULL;
    fname = (char *)malloc(strlen(fname_inp)+strlen(ext)+2);
    if (fname == NULL)
      return NULL;
    strcpy(fname,fname_inp);
    tmp1 = strrchr(fname, '/');
    tmp2 = strrchr(fname, '\\');
    if ((tmp1 == NULL) || (tmp1 < tmp2))
        tmp1 = tmp2;
    if (tmp1 == NULL)
        tmp1 = fname;
    tmp2 = strrchr(tmp1,'.');
    if ((tmp2 != NULL) && (tmp1+1 < tmp2))
    {
        sprintf(tmp2,".%s",ext);
    } else
    {
        tmp2 = fname + strlen(fname);
        sprintf(tmp2,".%s",ext);
    }
    return fname;
}

char *file_name_strip_to_body(const char *fname_inp)
{
    char *fname;
    const char *tmp1;
    char *tmp2;
    if (fname_inp == NULL)
      return NULL;
    tmp1 = strrchr(fname_inp, '/');
    tmp2 = strrchr(fname_inp, '\\');
    if ((tmp1 == NULL) || (tmp1 < tmp2))
        tmp1 = tmp2;
    if (tmp1 != NULL)
        tmp1++; // skip the '/' or '\\' char
    else
        tmp1 = fname_inp;
    fname = strdup(tmp1);
    if (fname == NULL)
      return NULL;
    tmp2 = strrchr(fname,'.');
    if ((tmp2 != NULL) && (fname+1 < tmp2))
    {
        *tmp2 = '\0';
    }
    return fname;
}

char *file_name_strip_path(const char *fname_inp)
{
    char *fname;
    const char *tmp1;
    char *tmp2;
    if (fname_inp == NULL)
      return NULL;
    tmp1 = strrchr(fname_inp, '/');
    tmp2 = strrchr(fname_inp, '\\');
    if ((tmp1 == NULL) || (tmp1 < tmp2))
        tmp1 = tmp2;
    if (tmp1 != NULL)
        tmp1++; // skip the '/' or '\\' char
    else
        tmp1 = fname_inp;
    fname = strdup(tmp1);
    if (fname == NULL)
      return NULL;
    return fname;
}

int load_command_line_options(ProgramOptions &opts, int argc, char *argv[])
{
    opts.clear();
    while (1)
    {
        static struct option long_options[] = {
            {"verbose", no_argument,       0, 'v'},
            {"output",  required_argument, 0, 'o'},
            {NULL,      0,                 0,'\0'}
        };
        /* getopt_long stores the option index here. */
        int c;
        int option_index = 0;
        c = getopt_long(argc, argv, "vo:", long_options, &option_index);
        /* Detect the end of the options. */
        if (c == -1)
            break;
        switch (c)
        {
        case 0:
               /* If this option set a flag, do nothing else now. */
               if (long_options[option_index].flag != 0)
                   break;
               printf ("option %s", long_options[option_index].name);
               if (optarg)
                 printf (" with arg %s", optarg);
               printf ("\n");
               break;
        case 'v':
            verbose++;
            break;
        case 'o':
            opts.fname_pal = optarg;
            break;
        case '?':
               // unrecognized option
               // getopt_long already printed an error message
        default:
            return false;
        }
    }
    // remaining command line arguments (not options)
    while (optind < argc)
    {
        opts.fnames_inp.push_back(argv[optind++]);
    }
    if ((optind < argc) || (opts.fnames_inp.size() < 1))
    {
        LogErr("Incorrectly specified input file name.");
        return false;
    }
    // fill names that were not set by arguments
    if (opts.fname_pal.length() < 1)
    {
        opts.fname_pal = file_name_change_extension(opts.fnames_inp[0].c_str(),"pal");
    }
    return true;
}

short show_head(void)
{
    LogMsg("\n%s (%s) %s",PROGRAM_FULL_NAME,PROGRAM_NAME,VER_STRING);
    LogMsg("  Created by %s; %s",PROGRAM_AUTHORS,LEGAL_COPYRIGHT);
    LogMsg("----------------------------------------");
    return ERR_OK;
}

/** Displays information about how to use this tool. */
short show_usage(char *fname)
{
    std::string xname = file_name_strip_path(fname);
    LogMsg("usage:\n");
    LogMsg("    %s [options] <filename>", xname.c_str());
    LogMsg("where <filename> should be the input BMP file, and [options] are:");
    LogMsg("    -v,--verbose             Verbose console output mode");
    LogMsg("    -o<file>,--output<file>  Output PAL file name");
    return ERR_OK;
}

short load_inp_png_file(ImageData& img, const std::string& fname_inp, ProgramOptions& opts)
{
    FILE* pngfile = fopen(fname_inp.c_str(),"rb");
    if (pngfile == NULL) {
        perror(fname_inp.c_str());
        return ERR_CANT_OPEN;
    }
    png_byte header[8];
    if (fread(header,8,1,pngfile) != 1) {
        perror(fname_inp.c_str());
        fclose(pngfile);
        return ERR_FILE_READ;
    }
    if (png_sig_cmp(header,0,8)) {
        LogErr("%s: Not a PNG file",fname_inp.c_str());
        fclose(pngfile);
        return ERR_BAD_FILE;
    }

    img.png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!img.png_ptr)
    {
        LogErr("%s: png_create_read_struct error",fname_inp.c_str());
        fclose(pngfile);
        return ERR_BAD_FILE;
    }

    img.info_ptr=png_create_info_struct(img.png_ptr);
    if (!img.info_ptr)
    {
        png_destroy_read_struct(&img.png_ptr, (png_infopp)NULL, (png_infopp)NULL);
        LogErr("%s: png_create_info_struct error",fname_inp.c_str());
        fclose(pngfile);
        return ERR_BAD_FILE;
    }

    img.end_info=png_create_info_struct(img.png_ptr);
    if (!img.end_info)
    {
        png_destroy_read_struct(&img.png_ptr, &img.info_ptr, (png_infopp)NULL);
        LogErr("%s: png_create_info_struct error",fname_inp.c_str());
        fclose(pngfile);
        return ERR_BAD_FILE;
    }

    if (setjmp(png_jmpbuf(img.png_ptr)))
    {
        png_destroy_read_struct(&img.png_ptr, &img.info_ptr, &img.end_info);
        LogErr("%s: PNG error",fname_inp.c_str());
        fclose(pngfile);
        exit(1);
    }

    png_init_io(img.png_ptr, pngfile);
    png_set_sig_bytes(img.png_ptr,8);
    int trafo=PNG_TRANSFORM_PACKING|PNG_TRANSFORM_STRIP_16|PNG_TRANSFORM_EXPAND;
    png_read_png(img.png_ptr, img.info_ptr, trafo , NULL);

    int bit_depth, interlace_type, compression_type, filter_method;
    png_get_IHDR(img.png_ptr, img.info_ptr, &img.width, &img.height, &bit_depth, &img.color_type,
        &interlace_type, &compression_type, &filter_method);

    if ((img.color_type & PNG_COLOR_MASK_COLOR)==0)
    {
        LogErr("%s: Grayscale image not supported",fname_inp.c_str());
        fclose(pngfile);
        return ERR_BAD_FILE;
    }

    fclose(pngfile);

    if (img.color_type==PNG_COLOR_TYPE_PALETTE)
    {
        LogErr("Invalid format. This shouldn't happen. PNG_TRANSFORM_EXPAND transforms image to RGB.");
        return ERR_BAD_FILE;
    }

    return ERR_OK;
}

short save_pal_file(WorkingSet& ws, const std::string& fname_pal, ProgramOptions& opts)
{
    // Open and write the PAL file
    {
        FILE* palfile = fopen(fname_pal.c_str(),"wb");
        if (palfile == NULL) {
            perror(fname_pal.c_str());
            return ERR_CANT_OPEN;
        }
        ColorPalette::iterator iter;
        for (iter = ws.palette.begin(); iter != ws.palette.end(); iter++)
        {
            char col[3];
            col[2] = (iter->red >> 2);
            col[1] = (iter->green >> 2);
            col[0] = (iter->blue >> 2);
            if (fwrite(col,3,1,palfile) != 1) {
                perror(fname_pal.c_str());
                return ERR_FILE_WRITE;
            }
        }
        fclose(palfile);
    }
    return ERR_OK;
}

int main(int argc, char* argv[])
{
    static ProgramOptions opts;
    if (!load_command_line_options(opts, argc, argv))
    {
        show_head();
        show_usage(argv[0]);
        return 11;
    }
    // start the work
    if (verbose)
        show_head();

    static WorkingSet ws;

    {
        std::vector<std::string>::iterator iter;
        for (iter = opts.fnames_inp.begin(); iter != opts.fnames_inp.end(); iter++)
        {
            ws.images.push_back(ImageData());
            if (load_inp_png_file(*ws.images.rend(), *iter, opts) != ERR_OK) {
                return 2;
            }
        }
    }
    {
        std::vector<ImageData>::iterator iter;
        ws.requestedColors(256);
        for (iter = ws.images.begin(); iter != ws.images.end(); iter++)
        {
            if (convert_rgb_to_indexed(ws, *iter, (((*iter).color_type & PNG_COLOR_MASK_ALPHA)!=0))) {
                LogErr("Warning! Color reduction may not be optimal!");
            }
            update_palette_colors_to_average_of_uses(ws);
        }
    }
    if (save_pal_file(ws, opts.fname_pal, opts) != ERR_OK) {
        return 8;
    }
    return 0;
}

