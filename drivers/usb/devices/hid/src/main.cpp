
#include <deque>
#include <experimental/optional>
#include <iostream>

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/await.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/client.hpp>

#include "hid.hpp"


// -----------------------------------------------------
// Fields.
// -----------------------------------------------------

struct Field {
	int bitOffset;
	int bitSize;
	uint16_t usagePage;
	uint16_t usageId;
};

std::vector<uint32_t> parse(std::vector<Field> fields, uint8_t *report) {
	std::vector<uint32_t> values;
	for(Field &f : fields) {
		int b = f.bitOffset / 8;
		uint32_t raw = uint32_t(report[b]) | (uint32_t(report[b + 1]) << 8)
				| (uint32_t(report[b + 2]) << 16) | (uint32_t(report[b + 3]) << 24);
		uint32_t mask = (uint32_t(1) << f.bitSize) - 1;
		values.push_back((raw >> (f.bitOffset % 8)) & mask);
	}
	return values;
}

std::vector<Field> fields;

uint32_t fetch(uint8_t *&p, void *limit, int n = 1) {
	uint32_t x = 0;
	for(int i = 0; i < n; i++) {
		x = (x << 8) | *p++;
		assert(p <= limit);
	}
	return x;
}

COFIBER_ROUTINE(async::result<void>, parseReportDescriptor(Device device, int index,
		int length, int intf_number), ([=] {
	arch::dma_object<SetupPacket> get_descriptor{device.setupPool()};
	get_descriptor->type = setup_type::targetInterface | setup_type::byStandard
			| setup_type::toHost;
	get_descriptor->request = request_type::getDescriptor;
	get_descriptor->value = (descriptor_type::report << 8) | index;
	get_descriptor->index = intf_number;
	get_descriptor->length = length;

	arch::dma_buffer buffer{device.bufferPool(), length};
	COFIBER_AWAIT device.transfer(ControlTransfer{kXferToHost,
			get_descriptor, buffer});

	int bit_offset = 0;

	std::experimental::optional<int> report_count;
	std::experimental::optional<int> report_size;
	std::experimental::optional<uint16_t> usage_page;
	std::deque<uint32_t> usage;
	std::experimental::optional<uint32_t> usage_min;
	std::experimental::optional<uint32_t> usage_max;

	auto p = reinterpret_cast<uint8_t *>(buffer.data());
	auto limit = reinterpret_cast<uint8_t *>(buffer.data()) + length;
	while(p < limit) {
		uint8_t token = fetch(p, limit);
		int size = (token & 0x03) == 3 ? 4 : (token & 0x03);
		uint32_t data = fetch(p, limit, size);
		switch(token & 0xFC) {
		// Main items
		case 0xC0:
			printf("End Collection: 0x%x\n", data);
			break;

		case 0xA0:
			printf("Collection: 0x%x\n", data);
			usage.clear();
			usage_min = std::experimental::nullopt;
			usage_max = std::experimental::nullopt;
			break;

		case 0x80:
			printf("Input: 0x%x\n", data);
			if(!report_size || !report_count)
				throw std::runtime_error("Missing Report Size/Count");
				
			if(!usage_min != !usage_max)
				throw std::runtime_error("Usage Minimum without Usage Maximum or visa versa");
			
			if(!usage.empty() && (usage_min || usage_max))
				throw std::runtime_error("Usage and Usage Mnimum/Maximum specified");

			if(usage.empty() && !usage_min && !usage_max) {
				// this field is just padding
				bit_offset += (*report_size) * (*report_count);
			}else{
				for(auto i = 0; i < *report_count; i++) {
					uint16_t actual_id;
					if(!usage.empty()) {
						actual_id = usage.front();
						usage.pop_front();
					}else{
						actual_id = *usage_min + i;
					}

					Field field;
					field.bitOffset = bit_offset;
					field.bitSize = *report_size;
					field.usagePage = *usage_page;
					field.usageId = actual_id;
					fields.push_back(field);
					
					bit_offset += *report_size;
				}

				usage.clear();
				usage_min = std::experimental::nullopt;
				usage_max = std::experimental::nullopt;
			}
			break;
		
		case 0x90:
			printf("Output: 0x%x\n", data);
			if(!report_size || !report_count)
				throw std::runtime_error("Missing Report Size/Count");
				
			if(!usage_min != !usage_max)
				throw std::runtime_error("Usage Minimum without Usage Maximum or visa versa");
			
			if(!usage.empty() && (usage_min || usage_max))
				throw std::runtime_error("Usage and Usage Mnimum/Maximum specified");

			if(usage.empty() && !usage_min && !usage_max) {
				// this field is just padding
				bit_offset += (*report_size) * (*report_count);
			}else{
				for(auto i = 0; i < *report_count; i++) {
					uint16_t actual_id;
					if(!usage.empty()) {
						actual_id = usage.front();
						usage.pop_front();
					}else{
						actual_id = *usage_min + i;
					}

					Field field;
					field.bitOffset = bit_offset;
					field.bitSize = *report_size;
					field.usagePage = *usage_page;
					field.usageId = actual_id;
					fields.push_back(field);
					
					bit_offset += *report_size;
				}

				usage.clear();
				usage_min = std::experimental::nullopt;
				usage_max = std::experimental::nullopt;
			}
			break;

		// Global items
		case 0x94:
			printf("Report Count: 0x%x\n", data);
			report_count = data;
			break;
		
		case 0x74:
			printf("Report Size: 0x%x\n", data);
			report_size = data;
			break;
		
		case 0x24:
			printf("Logical Maximum: 0x%x\n", data);
			break;
		
		case 0x14:
			printf("Logical Minimum: 0x%x\n", data);
			break;
		
		case 0x04:
			printf("Usage Page: 0x%x\n", data);
			usage_page = data;
			break;

		// Local items
		case 0x28:
			printf("Usage Maximum: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			usage_max = data;
			break;
		
		case 0x18:
			printf("Usage Minimum: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			usage_min = data;
			break;
			
		case 0x08:
			printf("Usage: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			usage.push_back(data);
			break;

		default:
			printf("Unexpected token: 0x%x\n", token & 0xFC);
			abort();
		}
	}
	
	COFIBER_RETURN();
}))

COFIBER_ROUTINE(cofiber::no_future, runHidDevice(Device device), ([=] {
	auto descriptor = COFIBER_AWAIT device.configurationDescriptor();

	std::experimental::optional<int> config_number;
	std::experimental::optional<int> intf_number;
	std::experimental::optional<int> in_endp_number;
	std::experimental::optional<int> report_desc_index;
	std::experimental::optional<int> report_desc_length;

	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		if(type == descriptor_type::configuration) {
			assert(!config_number);
			config_number = info.configNumber.value();
			
			auto desc = (ConfigDescriptor *)p;
		}else if(type == descriptor_type::interface) {
			assert(!intf_number);
			intf_number = info.interfaceNumber.value();
			
			auto desc = (InterfaceDescriptor *)p;
			assert(desc->interfaceClass == 3);
		}else if(type == descriptor_type::hid) {
			auto desc = (HidDescriptor *)p;
			assert(desc->length == sizeof(HidDescriptor) + (desc->numDescriptors * sizeof(HidDescriptor::Entry)));
			
			assert(info.interfaceNumber);
			
			for(size_t i = 0; i < desc->numDescriptors; i++) {
				assert(desc->entries[i].descriptorType == descriptor_type::report);
				assert(!report_desc_index);
				report_desc_index = 0;
				report_desc_length = desc->entries[i].descriptorLength;

			}
		}else if(type == descriptor_type::endpoint) {
			assert(!in_endp_number);
			in_endp_number = info.endpointNumber.value();
		}else{
			printf("Unexpected descriptor type: %d!\n", type);
		}
	});

	COFIBER_AWAIT parseReportDescriptor(device, report_desc_index.value(),
			report_desc_length.value(), intf_number.value());
	
	auto config = COFIBER_AWAIT device.useConfiguration(config_number.value());
	auto intf = COFIBER_AWAIT config.useInterface(intf_number.value(), 0);

	auto endp = COFIBER_AWAIT(intf.getEndpoint(PipeType::in, in_endp_number.value()));
	while(true) {
		arch::dma_buffer report{device.bufferPool(), 4};
		COFIBER_AWAIT endp.transfer(InterruptTransfer{XferFlags::kXferToHost, report});
	
		auto values = parse(fields, reinterpret_cast<uint8_t *>(report.data()));
		int counter = 0;
		for(uint32_t val : values) {
			printf("value %d: %x\n", counter, val);
			counter++;
		}
	}
}))

COFIBER_ROUTINE(cofiber::no_future, bindDevice(mbus::Entity entity), ([=] {
	auto lane = helix::UniqueLane(COFIBER_AWAIT entity.bind());
	auto device = protocols::usb::connect(std::move(lane));
	runHidDevice(device);
}))

COFIBER_ROUTINE(cofiber::no_future, observeDevices(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("usb.type", "device"),
		mbus::EqualsFilter("usb.class", "00")
	});
	COFIBER_AWAIT root.linkObserver(std::move(filter),
			[] (mbus::AnyEvent event) {
		if(event.type() == typeid(mbus::AttachEvent)) {
			std::cout << "uhci: Detected hid-device" << std::endl;
			bindDevice(boost::get<mbus::AttachEvent>(event).getEntity());
		}else{
			throw std::runtime_error("Unexpected device class");
		}
	});
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting hid (usb-)driver\n");

	observeDevices();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}

