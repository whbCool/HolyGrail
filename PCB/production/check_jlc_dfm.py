import os
import re
import json

def analyze_apertures_for_dfm(production_dir):
    gerber_dir = os.path.join(production_dir, 'Holy_Grail')
    if not os.path.exists(gerber_dir):
        return {"error": "Gerber directory not found"}

    dfm_results = {}
    
    # Files to check
    files_to_check = os.listdir(gerber_dir)
    
    # We will search for all ADD statements and check their sizes
    for filename in files_to_check:
        filepath = os.path.join(gerber_dir, filename)
        ext = os.path.splitext(filename)[1].lower()
        
        # Only process Gerber layer files
        if ext not in ['.gtl', '.gbl', '.g1', '.g2', '.g3', '.g4', '.gts', '.gbs', '.gto', '.gbo', '.gtp', '.gbp', '.gm1']:
            continue
            
        apertures = []
        min_trace_width = float('inf')
        min_via_pad = float('inf')
        
        # Read the file
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                
                # Check for Aperture Definition %ADD...*%
                # Examples:
                # %ADD66C,0.600000*%
                # %ADD68C,0.200000*%
                # %ADD10RoundRect,0.250000X-0.337500X-0.475000X...*%
                # %ADD28R,1.100000X1.300000*%
                if line.startswith('%AD'):
                    # Parse tool/aperture number and type
                    match = re.match(r'%ADD(\d+)([a-zA-Z0-9]+),([\d.X-]+)\*%', line)
                    if match:
                        ap_num = int(match.group(1))
                        ap_type = match.group(2)
                        ap_dims_str = match.group(3)
                        
                        # Parse dimensions
                        dims = []
                        for x in ap_dims_str.split('X'):
                            try:
                                dims.append(float(x))
                            except ValueError:
                                pass
                                
                        aperture_info = {
                            "number": ap_num,
                            "type": ap_type,
                            "dims": dims,
                            "raw": line
                        }
                        
                        # Find AperFunction comments preceding the definition to identify usage if available
                        # In KiCad 10.0.1, we see: G04 #@! TA.AperFunction,ViaPad* or Conductor*
                        # We don't have back-reference here, but we can look at the type.
                        # For circular apertures ('C'):
                        if ap_type == 'C' and len(dims) > 0:
                            size = dims[0]
                            # Check trace/conductor sizes (usually Conductor or trace)
                            # Typically conductors are drawn with small circles (e.g. 0.15mm - 0.5mm)
                            # Let's see if this could be a trace:
                            if size > 0.0:
                                apertures.append(aperture_info)
                                
                        else:
                            apertures.append(aperture_info)
                            
        dfm_results[filename] = {
            "apertures": apertures
        }
        
    return dfm_results

def check_drill_alignment(production_dir):
    # Parse drill files and compare with Gerber via pads
    gerber_dir = os.path.join(production_dir, 'Holy_Grail')
    
    pth_filepath = os.path.join(gerber_dir, 'DarkMagic-PTH.drl')
    npth_filepath = os.path.join(gerber_dir, 'DarkMagic-NPTH.drl')
    
    pth_tools = {}
    npth_tools = {}
    
    # Parse PTH drills
    if os.path.exists(pth_filepath):
        with open(pth_filepath, 'r') as f:
            for line in f:
                t_def_match = re.search(r'T(\d+)C([\d.]+)', line)
                if t_def_match:
                    tool_num = int(t_def_match.group(1))
                    tool_size = float(t_def_match.group(2))
                    pth_tools[tool_num] = tool_size
                    
    # Parse NPTH drills
    if os.path.exists(npth_filepath):
        with open(npth_filepath, 'r') as f:
            for line in f:
                t_def_match = re.search(r'T(\d+)C([\d.]+)', line)
                if t_def_match:
                    tool_num = int(t_def_match.group(1))
                    tool_size = float(t_def_match.group(2))
                    npth_tools[tool_num] = tool_size
                    
    return {
        "pth_drill_sizes": pth_tools,
        "npth_drill_sizes": npth_tools
    }

def main():
    production_dir = r'c:\Users\remas\Desktop\PCBs\DarkMagic\DarkMagic\production'
    ap_results = analyze_apertures_for_dfm(production_dir)
    drill_results = check_drill_alignment(production_dir)
    
    # Combine results
    report = {
        "apertures_by_file": ap_results,
        "drills": drill_results
    }
    
    scratch_dir = r'C:\Users\remas\.gemini\antigravity\brain\663ec088-413f-4f7c-a1e5-c87b9595ce2d\scratch'
    os.makedirs(scratch_dir, exist_ok=True)
    
    output_path = os.path.join(scratch_dir, 'jlc_dfm_check.json')
    with open(output_path, 'w') as f:
        json.dump(report, f, indent=4)
        
    print(f"Saved DFM analysis to {output_path}")

if __name__ == '__main__':
    main()
