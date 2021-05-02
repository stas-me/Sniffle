from math import floor

basePath = '/path/to/current/folder/'

inputFileNames = [
    'iphone_master/iphone_master_0_dbg',
    'iphone_master/iphone_master_1_dbg',
    'iphone_master/iphone_master_2_dbg',
    'iphone_master/iphone_master_3_dbg',
    'iphone_master/iphone_master_4_dbg',
    'iphone_master/iphone_master_5_dbg',
    'iphone_master/iphone_master_6_dbg',
    'iphone_master/iphone_master_7_dbg',
]

plotTemplate = basePath + 'plot_template.html'
outputPlot = basePath + 'iphone_master/plots_0_to_8_iphone.html'


stats = {
    'instant' : {
        'cu': {},
        'cm': {},
        'pu': {},
        'total': {}
    },
    'channelMap': {},
    'rangeSize': {},
    'numberOfRanges': {},
    'numberOfChannelsBetweenRanges': {},
    'phy': {
        '1m': 0,
        '2m': 0,
        'cod': 0,
    },
    'windowOffset': {},
    'windowSize': {},
    'interval': {},
    '12bytePduFrequency': {
        'chanMap': 0,
        'powerCon': 0,
        'powerChange': 0
    },
    'pduLength': {},
    'timeBetweenMapUpdates': {
        '0': 0,
        '1': 0,
        '2': 0,
        '3': 0,
        '4': 0,
        '5': 0,
        '6': 0,
        '7': 0,
        '8': 0,
        '9': 0,
        '10': 0,
        '11-14': 0,
        '14-17': 0,
        '17-20': 0,
        '20-30': 0,
        '30-40': 0,
        '40-60': 0,
        '60+': 0,
    },
}



def handle_interval(stats, val):
    if not val in stats['interval']:
        stats['interval'][val] = 0
    stats['interval'][val] += 1

    return stats


def handle_length(stats, val):
    if not val in stats['pduLength']:
        stats['pduLength'][val] = 0
    stats['pduLength'][val] += 1

    return stats


def handle_window_size(stats, val):
    if not val in stats['windowSize']:
        stats['windowSize'][val] = 0
    stats['windowSize'][val] += 1

    return stats


def handle_window_offset(stats, val):
    if not val in stats['windowOffset']:
        stats['windowOffset'][val] = 0
    stats['windowOffset'][val] += 1

    return stats


def handle_instant(stats, val, pduType):
    if not val in stats['instant'][pduType]:
        stats['instant'][pduType][val] = 0
    stats['instant'][pduType][val] += 1

    if not val in stats['instant']['total']:
        stats['instant']['total'][val] = 0
    stats['instant']['total'][val] += 1

    return stats


def handle_phy_mode(stats, val):
    stats['phy'][val] += 1

    return stats

def handle_channel_map(stats, map):
    map = int(map)
    binaryMap = "{0:b}".format(map)[::-1] # reverses the map
    print(binaryMap)
    if len(binaryMap) < 37:
        while len(binaryMap) != 37:
            binaryMap += '0'
    i = 0
    for c in binaryMap:
        if c == '0':
            stats['channelMap'][i] += 1
        i += 1

    ranges = []
    betweenRanges = []
    rangeOngoing = False
    thereWasARangeBefore = False
    rangeSize = 0
    betweenRangesSize = 0
    numberOfRanges = 0
    for c in (binaryMap + '1'): # + '1' is to close a range that may be in the end of the map
        if not rangeOngoing and c == '0':
            rangeOngoing = True
            numberOfRanges += 1
            if thereWasARangeBefore and betweenRangesSize > 0:
                betweenRanges.append(betweenRangesSize)
                betweenRangesSize = 0
        if c == '0':
            rangeSize += 1
        if rangeOngoing and c == '1':
            rangeOngoing = False
            thereWasARangeBefore = True
            ranges.append(rangeSize)
            rangeSize = 0
        if thereWasARangeBefore and not rangeOngoing:
            betweenRangesSize += 1

    if not str(numberOfRanges) in stats['numberOfRanges']:
        stats['numberOfRanges'][str(numberOfRanges)] = 0
    stats['numberOfRanges'][str(numberOfRanges)] += 1

    for rs in ranges:
        if not str(rs) in stats['rangeSize']:
            stats['rangeSize'][str(rs)] = 0
        stats['rangeSize'][str(rs)] += 1

    for rs in betweenRanges:
        if not str(rs) in stats['numberOfChannelsBetweenRanges']:
            stats['numberOfChannelsBetweenRanges'][str(rs)] = 0
        stats['numberOfChannelsBetweenRanges'][str(rs)] += 1
    
    return stats


def handle_map_update_diff(stats, prev, curr):
    if prev >= curr:
        return stats
    diff = floor(curr - prev)

    if diff <= 10:
        stats['timeBetweenMapUpdates'][str(diff)] += 1
    elif diff >= 11 and diff < 14:
        stats['timeBetweenMapUpdates']['11-14'] += 1
    elif diff >= 14 and diff < 17:
        stats['timeBetweenMapUpdates']['14-17'] += 1
    elif diff >= 17 and diff < 20:
        stats['timeBetweenMapUpdates']['17-20'] += 1
    elif diff >= 20 and diff < 30:
        stats['timeBetweenMapUpdates']['20-30'] += 1
    elif diff >= 30 and diff < 40:
        stats['timeBetweenMapUpdates']['30-40'] += 1
    elif diff >= 40 and diff < 60:
        stats['timeBetweenMapUpdates']['40-60'] += 1
    elif diff >= 60:
        stats['timeBetweenMapUpdates']['60+'] += 1

    return stats


def handle_12_byte_pdu(stats, pduType):
    if pduType == 'cu':
        stats['12bytePduFrequency']['chanMap'] += 1
    elif pduType == 'pcr':
        stats['12bytePduFrequency']['powerCon'] += 1
    elif pduType == 'pci':
        stats['12bytePduFrequency']['powerChange'] += 1

    return stats

def generate_plots(stats, plotTemplate, outputPlot):
    output = open(outputPlot, "w")
    template = open(plotTemplate)

    for line in template:
        if '{{channel-map-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['channelMap'])
            line = line.replace('{{channel-map-x}}', keys)
        elif '{{channel-map-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['channelMap'])
            line = line.replace('{{channel-map-y}}', vals)
        
        elif '{{instant-num-per-conn-upd-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['instant']['cu'])
            line = line.replace('{{instant-num-per-conn-upd-x}}', keys)
        elif '{{instant-num-per-conn-upd-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['instant']['cu'])
            line = line.replace('{{instant-num-per-conn-upd-y}}', vals)
        
        elif '{{instant-num-per-map-upd-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['instant']['cm'])
            line = line.replace('{{instant-num-per-map-upd-x}}', keys)
        elif '{{instant-num-per-map-upd-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['instant']['cm'])
            line = line.replace('{{instant-num-per-map-upd-y}}', vals)
        
        elif '{{instant-num-per-phy-upd-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['instant']['pu'])
            line = line.replace('{{instant-num-per-phy-upd-x}}', keys)        
        elif '{{instant-num-per-phy-upd-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['instant']['pu'])
            line = line.replace('{{instant-num-per-phy-upd-y}}', vals)
        
        elif '{{instant-num-total-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['instant']['total'])
            line = line.replace('{{instant-num-total-x}}', keys)
        elif '{{instant-num-total-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['instant']['total'])
            line = line.replace('{{instant-num-total-y}}', vals)
        
        elif '{{size-of-map-range-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['rangeSize'])
            line = line.replace('{{size-of-map-range-x}}', keys)
        elif '{{size-of-map-range-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['rangeSize'])
            line = line.replace('{{size-of-map-range-y}}', vals)
        
        elif '{{number-of-map-ranges-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['numberOfRanges'])
            line = line.replace('{{number-of-map-ranges-x}}', keys)
        elif '{{number-of-map-ranges-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['numberOfRanges'])
            line = line.replace('{{number-of-map-ranges-y}}', vals)
        
        elif '{{number-of-channels-between-ranges-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['numberOfChannelsBetweenRanges'])
            line = line.replace('{{number-of-channels-between-ranges-x}}', keys)
        elif '{{number-of-channels-between-ranges-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['numberOfChannelsBetweenRanges'])
            line = line.replace('{{number-of-channels-between-ranges-y}}', vals)
        
        elif '{{phy-modes-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['phy'])
            line = line.replace('{{phy-modes-y}}', vals)
        
        elif '{{win-offset-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['windowOffset'])
            line = line.replace('{{win-offset-x}}', keys)
        elif '{{win-offset-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['windowOffset'])
            line = line.replace('{{win-offset-y}}', vals)
        
        elif '{{win-size-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['windowSize'])
            line = line.replace('{{win-size-x}}', keys)
        elif '{{win-size-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['windowSize'])
            line = line.replace('{{win-size-y}}', vals)
        
        elif '{{interval-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['interval'])
            line = line.replace('{{interval-x}}', keys)
        elif '{{interval-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['interval'])
            line = line.replace('{{interval-y}}', vals)
        
        elif '{{12-byte-pdus-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['12bytePduFrequency'])
            line = line.replace('{{12-byte-pdus-y}}', vals)
        
        elif '{{pdu-length-frequency-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['pduLength'])
            line = line.replace('{{pdu-length-frequency-x}}', keys)
        elif '{{pdu-length-frequency-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['pduLength'])
            line = line.replace('{{pdu-length-frequency-y}}', vals)
        
        elif '{{time-between-map-updates-x}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['timeBetweenMapUpdates'])
            line = line.replace('{{time-between-map-updates-x}}', keys)
        elif '{{time-between-map-updates-y}}' in line:
            keys, vals = separate_and_prepare_keys_and_vals(stats['timeBetweenMapUpdates'])
            line = line.replace('{{time-between-map-updates-y}}', vals)

        output.write(line)

    output.close()
    template.close()

def separate_and_prepare_keys_and_vals(oooo):
    
    keysSorted = []
    ignoreSorting = False
    copiedWithStringKeys = {}
    for key, val in oooo.items():
        try:
            copiedWithStringKeys[str(key)] = oooo[key]
            keysSorted.append(int(key))
        except:
            keysSorted.append(key)
            ignoreSorting = True
    if not ignoreSorting:
        keysSorted.sort()


    keys = []
    vals = []

    for key in keysSorted:
        # These are not spaces https://www.compart.com/en/unicode/U+2800.
        # It is used to force plotly to treat these labels as strings, instead of integers.
        keys.append('\'⠀' + str(key) + '⠀\'')
        vals.append(str(copiedWithStringKeys[str(key)]))

    return ', '.join(keys), ', '.join(vals)












for i in range(0, 37):
    stats['channelMap'][i] = 0

print(stats)

for fn in inputFileNames:
    sourceFileName = basePath + fn

    filehandler = open(sourceFileName)
    opcodeParams = {}
    for line in filehandler:
        line = line.rstrip()
        if line == '---SESSION_START---' or len(line) < 3:
            lastMapUpdateTimestamp = 0
            continue
        parts = line.split('|');
        pduType = ''
        for part in parts:
            dataPiece = part.split(':')
            key = dataPiece[0]
            val = dataPiece[1]

            if key == 't':
                pduType = val

            if pduType == 'con':
                lastMapUpdateTimestamp = 0

            if key == 'int':
                stats = handle_interval(stats, val)
            if key == 'm':
                stats = handle_channel_map(stats, val)
            if key == 'len':
                stats = handle_length(stats, val)
            if key == 'ws':
                stats = handle_window_size(stats, val)
            if key == 'wo':
                stats = handle_window_offset(stats, val)
            if key == 'ins':
                stats = handle_instant(stats, val, pduType)
            if key == 'phy':
                stats = handle_phy_mode(stats, val)
            if key == 'ts' and pduType == 'cm':
                if lastMapUpdateTimestamp == 0:
                    lastMapUpdateTimestamp = float(val)
                else:
                    stats = handle_map_update_diff(stats, lastMapUpdateTimestamp, float(val))
                    lastMapUpdateTimestamp = float(val)


            if key == 't':
                if pduType == 'cu' or pduType == 'pcr' or pduType == 'pci':
                    stats = handle_12_byte_pdu(stats, pduType)
                    stats = handle_length(stats, '12')
                if pduType == 'cm':
                    stats = handle_length(stats, '8')
                if pduType == 'pu':
                    stats = handle_length(stats, '5')
    filehandler.close()

generate_plots(stats, plotTemplate, outputPlot)



