import os
import re
import json

def parse_gerber_coordinates(line, x_scale, y_scale):
    # Regex to find X and Y coordinate patterns
    x_match = re.search(r'X([+-]?\d+)', line)
    y_match = re.search(r'Y([+-]?\d+)', line)
    x = int(x_match.group(1)) * x_scale if x_match else None
    y = int(int(y_match.group(1)) * y_scale) if y_match else None
    return x, y

def analyze_gerber_file(filepath):
    filename = os.path.basename(filepath)
    filesize = os.path.getsize(filepath)
    
    # Initialize statistics
    metadata = {}
    d01_count = 0
    d02_count = 0
    d03_count = 0
    polygon_count = 0
    aperture_defs = []
    
    x_coords = []
    y_coords = []
    
    # Parse format specs
    # Default format: 4.6 (which means scale by 10^-6)
    x_scale = 1e-6
    y_scale = 1e-6
    unit = 'mm'
    
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        in_polygon = False
        for line in f:
            line = line.strip()
            if not line:
                continue
                
            # Parse metadata comments
            if line.startswith('G04'):
                # Generation Software
                if 'TF.GenerationSoftware' in line:
                    parts = line.split(',')
                    metadata['software'] = parts[1] if len(parts) > 1 else ''
                elif 'TF.CreationDate' in line:
                    metadata['creation_date'] = line.split(',')[1].replace('*', '') if len(line.split(',')) > 1 else ''
                elif 'TF.ProjectId' in line:
                    metadata['project_id'] = line.split(',')[1].replace('*', '') if len(line.split(',')) > 1 else ''
                elif 'TF.FileFunction' in line:
                    metadata['file_function'] = line.split(',')[1:]
                continue
            
            # Units
            if '%MOIN*%' in line:
                unit = 'inch'
            elif '%MOMM*%' in line:
                unit = 'mm'
                
            # Format specification
            fs_match = re.search(r'%FS[L|T][A|I]X(\d)(\d)Y\d\d\*%', line)
            if fs_match:
                # e.g., FSLAX46Y46*% -> integer digits 4, decimal digits 6
                # Scale is 10^(-decimal_digits)
                dec_digits = int(fs_match.group(2))
                x_scale = 10**(-dec_digits)
                y_scale = 10**(-dec_digits)
                
            # Aperture definitions
            if line.startswith('%AD'):
                aperture_defs.append(line)
                
            # Polygon modes
            if 'G36*' in line:
                in_polygon = True
                polygon_count += 1
            elif 'G37*' in line:
                in_polygon = False
                
            # Gerber Commands
            if 'D01*' in line or line.endswith('D01'):
                d01_count += 1
            elif 'D02*' in line or line.endswith('D02'):
                d02_count += 1
            elif 'D03*' in line or line.endswith('D03'):
                d03_count += 1
                
            # Extract coordinates for bounding box (especially important for Edge_Cuts)
            if 'X' in line or 'Y' in line:
                x, y = parse_gerber_coordinates(line, x_scale, y_scale)
                if x is not None:
                    x_coords.append(x)
                if y is not None:
                    y_coords.append(y)
                    
    # Calculate bounds
    min_x = min(x_coords) if x_coords else 0.0
    max_x = max(x_coords) if x_coords else 0.0
    min_y = min(y_coords) if y_coords else 0.0
    max_y = max(y_coords) if y_coords else 0.0
    
    width = max_x - min_x
    height = max_y - min_y
    
    # Adjust for units (if inches, convert to mm for standard report, or report both)
    width_mm = width if unit == 'mm' else width * 25.4
    height_mm = height if unit == 'mm' else height * 25.4
    
    return {
        'filename': filename,
        'size_bytes': filesize,
        'metadata': metadata,
        'units': unit,
        'bounds': {
            'min_x': min_x,
            'max_x': max_x,
            'min_y': min_y,
            'max_y': max_y,
            'width': width,
            'height': height,
            'width_mm': width_mm,
            'height_mm': height_mm
        },
        'commands': {
            'draw_d01': d01_count,
            'move_d02': d02_count,
            'flash_d03': d03_count,
            'polygons': polygon_count
        },
        'apertures_count': len(aperture_defs)
    }

def analyze_drill_file(filepath):
    filename = os.path.basename(filepath)
    filesize = os.path.getsize(filepath)
    
    tools = {}
    hole_count = 0
    slot_count = 0
    
    unit = 'mm'
    current_tool = None
    
    # Parse Excellon file
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        in_header = False
        for line in f:
            line = line.strip()
            if not line:
                continue
                
            if line == 'M48':
                in_header = True
                continue
            if line == '%':
                in_header = False
                continue
                
            # Parse units
            if 'METRIC' in line:
                unit = 'mm'
            elif 'INCH' in line:
                unit = 'inch'
                
            # Parse tool definitions
            # e.g., T1C0.300 or T01C0.3
            t_def_match = re.search(r'T(\d+)C([\d.]+)', line)
            if t_def_match:
                tool_num = int(t_def_match.group(1))
                tool_size = float(t_def_match.group(2))
                tools[tool_num] = {
                    'size': tool_size,
                    'count': 0,
                    'slots_count': 0,
                    'function': ''
                }
                continue
                
            # Select tool
            t_sel_match = re.match(r'^T(\d+)(?!\w)', line)
            if t_sel_match:
                current_tool = int(t_sel_match.group(1))
                continue
                
            # Coordinate lines
            if line.startswith('X') or line.startswith('Y'):
                if current_tool in tools:
                    tools[current_tool]['count'] += 1
                    hole_count += 1
                    
            # Parse slots (G00 / G01 routing)
            if 'G00' in line or 'G01' in line:
                if current_tool in tools:
                    # Let's count routing paths
                    if 'G01' in line:
                        tools[current_tool]['slots_count'] += 1
                        slot_count += 1
                        
    return {
        'filename': filename,
        'size_bytes': filesize,
        'units': unit,
        'hole_count': hole_count,
        'slot_count': slot_count,
        'tools': tools
    }

def main():
    production_dir = r'c:\Users\remas\Desktop\PCBs\DarkMagic\DarkMagic\production'
    gerber_dir = os.path.join(production_dir, 'Holy_Grail')
    
    if not os.path.exists(gerber_dir):
        print(f"Directory {gerber_dir} does not exist.")
        return
        
    all_files = os.listdir(gerber_dir)
    print(f"Found {len(all_files)} files in Gerber directory.")
    
    results = {
        'gerbers': {},
        'drills': {}
    }
    
    for filename in all_files:
        filepath = os.path.join(gerber_dir, filename)
        ext = os.path.splitext(filename)[1].lower()
        
        # Analyze Gerber files
        if ext in ['.gbr', '.gtl', '.gbl', '.gts', '.gbs', '.gto', '.gbo', '.gtp', '.gbp', '.gm1', '.g1', '.g2', '.g3', '.g4']:
            print(f"Analyzing Gerber: {filename}")
            results['gerbers'][filename] = analyze_gerber_file(filepath)
            
        # Analyze Excellon drill files
        elif ext in ['.drl', '.txt']:
            print(f"Analyzing Drill: {filename}")
            results['drills'][filename] = analyze_drill_file(filepath)
            
    # Save results to json file in scratch
    scratch_dir = r'C:\Users\remas\.gemini\antigravity\brain\663ec088-413f-4f7c-a1e5-c87b9595ce2d\scratch'
    os.makedirs(scratch_dir, exist_ok=True)
    
    output_path = os.path.join(scratch_dir, 'gerber_analysis.json')
    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump(results, f, indent=4)
        
    print(f"Saved analysis results to {output_path}")

if __name__ == '__main__':
    main()
