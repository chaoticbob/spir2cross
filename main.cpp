/*
 * Copyright 2015-2016 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "spir2cpp.hpp"
#include <cstdio>
#include <stdexcept>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <cstring>
#include <unordered_set>

using namespace spv;
using namespace spir2cross;
using namespace std;

struct CLIParser;
struct CLICallbacks
{
    void add(const char *cli, const function<void (CLIParser&)> &func)
    {
        callbacks[cli] = func;
    }
    unordered_map<string, function<void (CLIParser&)>> callbacks;
    function<void ()> error_handler;
    function<void (const char*)> default_handler;
};

struct CLIParser
{
    CLIParser(CLICallbacks cbs, int argc, char *argv[])
        : cbs(move(cbs)), argc(argc), argv(argv)
    {}

    bool parse()
    {
        try
        {
            while (argc && !ended_state)
            {
                const char *next = *argv++;
                argc--;

                if (*next != '-' && cbs.default_handler)
                {
                    cbs.default_handler(next);
                }
                else
                {
                    auto itr = cbs.callbacks.find(next);
                    if (itr == ::end(cbs.callbacks))
                    {
                        throw logic_error("Invalid argument.\n");
                    }

                    itr->second(*this);
                }
            }

            return true;
        }
        catch (...)
        {
            if (cbs.error_handler)
            {
                cbs.error_handler();
            }
            return false;
        }
    }

    void end()
    {
        ended_state = true;
    }

    uint32_t next_uint()
    {
        if (!argc)
        {
            throw logic_error("Tried to parse uint, but nothing left in arguments.\n");
        }

        uint32_t val = stoul(*argv);
        if (val > numeric_limits<uint32_t>::max())
        {
            throw out_of_range("next_uint() out of range.\n");
        }

        argc--;
        argv++;

        return val;
    }

    double next_double()
    {
        if (!argc)
        {
            throw logic_error("Tried to parse double, but nothing left in arguments.\n");
        }

        double val = stod(*argv);

        argc--;
        argv++;

        return val;
    }

    const char *next_string()
    {
        if (!argc)
        {
            throw logic_error("Tried to parse string, but nothing left in arguments.\n");
        }

        const char *ret = *argv;
        argc--;
        argv++;
        return ret;
    }

    CLICallbacks cbs;
    int argc;
    char **argv;
    bool ended_state = false;
};

static vector<uint32_t> read_spirv_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
    {
        fprintf(stderr, "Failed to open SPIRV file: %s\n", path);
        return {};
    }

    fseek(file, 0, SEEK_END);
    long len = ftell(file) / sizeof(uint32_t);
    rewind(file);

    vector<uint32_t> spirv(len);
    if (fread(spirv.data(), sizeof(uint32_t), len, file) != size_t(len))
        spirv.clear();

    fclose(file);
    return spirv;
}

static bool write_string_to_file(const char *path, const char *string)
{
    FILE *file = fopen(path, "w");
    if (!file)
    {
        fprintf(file, "Failed to write file: %s\n", path);
        return false;
    }

    fprintf(file, "%s", string);
    fclose(file);
    return true;
}

static void print_resources(const Compiler &compiler, const char *tag, const vector<Resource> &resources)
{
    fprintf(stderr, "%s\n", tag);
    fprintf(stderr, "=============\n\n");
    for (auto &res : resources)
    {
        auto &type = compiler.get_type(res.type_id);
        auto mask = compiler.get_decoration_mask(res.id);

        // If we don't have a name, use the fallback for the type instead of the variable
        // for SSBOs and UBOs since those are the only meaningful names to use externally.
        // Push constant blocks are still accessed by name and not block name, even though they are technically Blocks.
        bool is_push_constant = compiler.get_storage_class(res.id) == StorageClassPushConstant;
        bool is_block = (compiler.get_decoration_mask(type.self) &
                ((1ull << DecorationBlock) | (1ull << DecorationBufferBlock))) != 0;
        uint32_t fallback_id = !is_push_constant && is_block ? res.type_id : res.id;

        fprintf(stderr, " ID %03u : %s", res.id,
                !res.name.empty() ? res.name.c_str() : compiler.get_fallback_name(fallback_id).c_str());

        if (mask & (1ull << DecorationLocation))
            fprintf(stderr, " (Location : %u)", compiler.get_decoration(res.id, DecorationLocation));
        if (mask & (1ull << DecorationDescriptorSet))
            fprintf(stderr, " (Set : %u)", compiler.get_decoration(res.id, DecorationDescriptorSet));
        if (mask & (1ull << DecorationBinding))
            fprintf(stderr, " (Binding : %u)", compiler.get_decoration(res.id, DecorationBinding));
        fprintf(stderr, "\n");

		if((! is_push_constant) && is_block) {
			for(uint32_t index = 0; index < compiler.get_member_count(res.type_id); ++index) {
				fprintf(stderr, "    %u: %s (%u)\n", index, compiler.get_member_name(res.type_id, index).c_str(), compiler.get_member_decoration(res.type_id, index, DecorationOffset));
			}
		}
    }
    fprintf(stderr, "=============\n\n");
}

static void print_resources(const Compiler &compiler, const ShaderResources &res)
{
    print_resources(compiler, "subpass inputs", res.subpass_inputs);
    print_resources(compiler, "inputs", res.stage_inputs);
    print_resources(compiler, "outputs", res.stage_outputs);
    print_resources(compiler, "textures", res.sampled_images);
    print_resources(compiler, "images", res.storage_images);
    print_resources(compiler, "ssbos", res.storage_buffers);
    print_resources(compiler, "ubos", res.uniform_buffers);
    print_resources(compiler, "push", res.push_constant_buffers);
    print_resources(compiler, "counters", res.atomic_counters);
}

static void print_push_constant_resources(const Compiler &compiler, const vector<Resource> &res)
{
    for (auto &block : res)
    {
        auto ranges = compiler.get_active_buffer_ranges(block.id);
        fprintf(stderr, "Active members in buffer: %s\n",
                !block.name.empty() ? block.name.c_str() : compiler.get_fallback_name(block.id).c_str());

        fprintf(stderr, "==================\n\n");
        for (auto &range : ranges)
        {
            const auto &name = compiler.get_member_name(block.type_id, range.index);

            fprintf(stderr, "Member #%3u (%s): Offset: %4u, Range: %4u\n",
                    range.index, !name.empty() ? name.c_str() : compiler.get_fallback_member_name(range.index).c_str(),
                    unsigned(range.offset), unsigned(range.range));
        }
        fprintf(stderr, "==================\n\n");
    }
}

struct PLSArg
{
    PlsFormat format;
    string name;
};

struct CLIArguments
{
    const char *input = nullptr;
    const char *output = nullptr;
    uint32_t version = 0;
    bool es = false;
    bool set_version = false;
    bool set_es = false;
    bool dump_resources = false;
    bool force_temporary = false;
    bool flatten_ubo = false;
    bool fixup = false;
    vector<PLSArg> pls_in;
    vector<PLSArg> pls_out;

    uint32_t iterations = 1;
    bool cpp = false;
};

static void print_help()
{
    fprintf(stderr, "Usage: spir2cross [--output <output path>] [SPIR-V file] [--es] [--no-es] [--version <GLSL version>] [--dump-resources] [--help] [--force-temporary] [-cpp] [--flatten-ubo] [--fixup-clipspace] [--iterations iter] [--pls-in format input-name] [--pls-out format output-name]\n");
}

static vector<PlsRemap> remap_pls(const vector<PLSArg> &pls_variables, const vector<Resource> &resources, const vector<Resource> *secondary_resources)
{
    vector<PlsRemap> ret;

    for (auto &pls : pls_variables)
    {
        bool found = false;
        for (auto &res : resources)
        {
            if (res.name == pls.name)
            {
                ret.push_back({ res.id, pls.format });
                found = true;
                break;
            }
        }

        if (!found && secondary_resources)
        {
            for (auto &res : *secondary_resources)
            {
                if (res.name == pls.name)
                {
                    ret.push_back({ res.id, pls.format });
                    found = true;
                    break;
                }
            }
        }

        if (!found)
            fprintf(stderr, "Did not find stage input/output/target with name \"%s\".\n",
                    pls.name.c_str());
    }

    return ret;
}

static PlsFormat pls_format(const char *str)
{
    if (!strcmp(str, "r11f_g11f_b10f")) return PlsR11FG11FB10F;
    else if (!strcmp(str, "r32f")) return PlsR32F;
    else if (!strcmp(str, "rg16f")) return PlsRG16F;
    else if (!strcmp(str, "rg16")) return PlsRG16;
    else if (!strcmp(str, "rgb10_a2")) return PlsRGB10A2;
    else if (!strcmp(str, "rgba8")) return PlsRGBA8;
    else if (!strcmp(str, "rgba8i")) return PlsRGBA8I;
    else if (!strcmp(str, "rgba8ui")) return PlsRGBA8UI;
    else if (!strcmp(str, "rg16i")) return PlsRG16I;
    else if (!strcmp(str, "rgb10_a2ui")) return PlsRGB10A2UI;
    else if (!strcmp(str, "rg16ui")) return PlsRG16UI;
    else if (!strcmp(str, "r32ui")) return PlsR32UI;
    else return PlsNone;
}

int main(int argc, char *argv[])
{
    CLIArguments args;
    CLICallbacks cbs;

    cbs.add("--help",            [](CLIParser &parser) { print_help(); parser.end(); });
    cbs.add("--output",          [&args](CLIParser &parser) { args.output = parser.next_string(); });
    cbs.add("--es",              [&args](CLIParser &)       { args.es = true; args.set_es = true; });
    cbs.add("--no-es",           [&args](CLIParser &)       { args.es = false; args.set_es = true; });
    cbs.add("--version",         [&args](CLIParser &parser) { args.version = parser.next_uint(); args.set_version = true; });
    cbs.add("--dump-resources",  [&args](CLIParser &)       { args.dump_resources = true; });
    cbs.add("--force-temporary", [&args](CLIParser &)       { args.force_temporary = true; });
    cbs.add("--flatten-ubo",     [&args](CLIParser &)       { args.flatten_ubo = true; });
    cbs.add("--fixup-clipspace", [&args](CLIParser &)       { args.fixup = true; });
    cbs.add("--iterations",      [&args](CLIParser &parser) { args.iterations = parser.next_uint(); });
    cbs.add("--cpp",             [&args](CLIParser &)       { args.cpp = true; });

    cbs.add("--pls-in", [&args](CLIParser &parser) {
        auto fmt = pls_format(parser.next_string());
        auto name = parser.next_string();
        args.pls_in.push_back({ move(fmt), move(name) });
    });
    cbs.add("--pls-out", [&args](CLIParser &parser) {
        auto fmt = pls_format(parser.next_string());
        auto name = parser.next_string();
        args.pls_out.push_back({ move(fmt), move(name) });
    });

    cbs.default_handler = [&args](const char *value) { args.input = value; };
    cbs.error_handler = []{ print_help(); };

    CLIParser parser{move(cbs), argc - 1, argv + 1};
    if (!parser.parse())
    {
        return EXIT_FAILURE;
    }
    else if (parser.ended_state)
    {
        return EXIT_SUCCESS;
    }

    if (!args.input)
    {
        fprintf(stderr, "Didn't specify input file.\n");
        print_help();
        return EXIT_FAILURE;
    }

    unique_ptr<CompilerGLSL> compiler;

    if (args.cpp)
        compiler = unique_ptr<CompilerGLSL>(new CompilerCPP(read_spirv_file(args.input)));
    else
        compiler = unique_ptr<CompilerGLSL>(new CompilerGLSL(read_spirv_file(args.input)));

    if (!args.set_version && !compiler->get_options().version)
    {
        fprintf(stderr, "Didn't specify GLSL version and SPIR-V did not specify language.\n");
        print_help();
        return EXIT_FAILURE;
    }

    CompilerGLSL::Options opts = compiler->get_options();
    if (args.set_version)
        opts.version = args.version;
    if (args.set_es)
        opts.es = args.es;
    opts.force_temporary = args.force_temporary;
    opts.vertex.fixup_clipspace = args.fixup;
    compiler->set_options(opts);

    auto res = compiler->get_shader_resources();

    if (args.flatten_ubo)
        for (auto &ubo : res.uniform_buffers)
            compiler->flatten_interface_block(ubo.id);

    auto pls_inputs = remap_pls(args.pls_in, res.stage_inputs, &res.subpass_inputs);
    auto pls_outputs = remap_pls(args.pls_out, res.stage_outputs, nullptr);
    compiler->remap_pixel_local_storage(move(pls_inputs), move(pls_outputs));

    if (args.dump_resources)
    {
        print_resources(*compiler, res);
        print_push_constant_resources(*compiler, res.push_constant_buffers);
    }

    string glsl;
    for (uint32_t i = 0; i < args.iterations; i++)
        glsl = compiler->compile();

    if (args.output)
        write_string_to_file(args.output, glsl.c_str());
    else
        printf("%s", glsl.c_str());
}

