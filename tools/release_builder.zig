const std = @import("std");

pub fn main() !void {
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const allocator = arena.allocator();

    const args = try std.process.argsAlloc(allocator);
    if (args.len != 2) {
        std.debug.print("Usage: {s} <version> (e.g. v0.1.0)\n", .{args[0]});
        std.process.exit(1);
    }
    const version = args[1];

    // Build environment and paths
    const build_env = "paper_s3_release";
    
    // Get current working directory
    var buf: [1024]u8 = undefined;
    const cwd = try std.process.getCwd(&buf);
    
    // Define paths
    const build_dir_path = try std.fs.path.join(allocator, &[_][]const u8{ cwd, ".pio", "build", build_env });
    const releases_dir_path = try std.fs.path.join(allocator, &[_][]const u8{ cwd, "releases" });
    const release_name = try std.fmt.allocPrint(allocator, "paper_s3_{s}", .{version});
    const release_dir_path = try std.fs.path.join(allocator, &[_][]const u8{ releases_dir_path, release_name });
    const zip_name = try std.fmt.allocPrint(allocator, "paper_s3_{s}.zip", .{version});

    // Create release directory
    try std.fs.cwd().makePath(release_dir_path);

    std.debug.print("[release_builder] Building environment: {s}\n", .{build_env});

    // Run pio run
    var pio_cmd = std.process.Child.init(&[_][]const u8{ "pio", "run", "-e", build_env }, allocator);
    pio_cmd.cwd = cwd;
    const term = try pio_cmd.spawnAndWait();
    if (term != .Exited or term.Exited != 0) {
        std.debug.print("Error: pio build failed\n", .{});
        std.process.exit(1);
    }

    std.debug.print("[release_builder] Collecting build artifacts from {s}\n", .{build_dir_path});

    // Artifacts to copy
    const bootloader_bin = try std.fs.path.join(allocator, &[_][]const u8{ build_dir_path, "bootloader.bin" });
    const firmware_bin = try std.fs.path.join(allocator, &[_][]const u8{ build_dir_path, "firmware.bin" });
    const partitions_bin = try std.fs.path.join(allocator, &[_][]const u8{ build_dir_path, "partitions.bin" });
    const partition_table_bin = try std.fs.path.join(allocator, &[_][]const u8{ build_dir_path, "partition-table.bin" });

    // Helper function to copy file
    const copyFile = struct {
        fn call(mem_allocator: std.mem.Allocator, from: []const u8, to_dir: []const u8, name: []const u8) !void {
            const dest_path = try std.fs.path.join(mem_allocator, &[_][]const u8{ to_dir, name });
            std.debug.print("Copying {s} -> {s}\n", .{from, dest_path});
            try std.fs.copyFileAbsolute(from, dest_path, .{});
        }
    }.call;

    // Check and copy files
    try copyFile(allocator, bootloader_bin, release_dir_path, "bootloader.bin");
    try copyFile(allocator, firmware_bin, release_dir_path, "firmware.bin");

    // Handle partitions binary (check both names as in original script)
    const partitions_src = blk: {
        std.fs.accessAbsolute(partitions_bin, .{}) catch {
            std.fs.accessAbsolute(partition_table_bin, .{}) catch {
                std.debug.print("Error: No partition image found at {s} or {s}\n", .{partitions_bin, partition_table_bin});
                std.process.exit(1);
            };
            break :blk partition_table_bin;
        };
        break :blk partitions_bin;
    };
    try copyFile(allocator, partitions_src, release_dir_path, std.fs.path.basename(partitions_src));

    // Copy user guide
    const guide_src = try std.fs.path.join(allocator, &[_][]const u8{ cwd, "docs", "USER_GUIDE_PAPER_S3.md" });
    copyFile(allocator, guide_src, release_dir_path, "USER_GUIDE_PAPER_S3.md") catch |err| {
        std.debug.print("WARNING: Could not copy user guide: {}\n", .{err});
    };

    // Zip
    std.debug.print("[release_builder] Creating archive {s}\n", .{zip_name});
    
    // Remove existing zip
    const zip_path = try std.fs.path.join(allocator, &[_][]const u8{ releases_dir_path, zip_name });
    std.fs.deleteFileAbsolute(zip_path) catch {}; // Ignore if doesn't exist

    // Run zip command
    // zip -r "<zip_name>" "<release_dir_cms>"
    var zip_cmd = std.process.Child.init(&[_][]const u8{ "zip", "-r", zip_name, release_name }, allocator);
    zip_cmd.cwd = releases_dir_path; // Run inside releases dir so paths in zip are relative
    zip_cmd.stdout_behavior = .Ignore; // Suppress stdout like >/dev/null
    const zip_term = try zip_cmd.spawnAndWait();
    if (zip_term != .Exited or zip_term.Exited != 0) {
        std.debug.print("Error: zip command failed\n", .{});
        std.process.exit(1);
    }

    std.debug.print("[release_builder] Done. Release archive: {s}\n", .{zip_path});
}
