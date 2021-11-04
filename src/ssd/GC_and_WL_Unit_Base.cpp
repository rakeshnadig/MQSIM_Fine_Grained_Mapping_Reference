#include "GC_and_WL_Unit_Base.h"
#include <vector>

namespace SSD_Components
{
	GC_and_WL_Unit_Base* GC_and_WL_Unit_Base::_my_instance;
	
	GC_and_WL_Unit_Base::GC_and_WL_Unit_Base(const sim_object_id_type& id,
		Address_Mapping_Unit_Base* address_mapping_unit, 
		Flash_Block_Manager_Base* block_manager, 
		TSU_Base* tsu, 
		NVM_PHY_ONFI* flash_controller,
		GC_Block_Selection_Policy_Type block_selection_policy, 
		double gc_threshold, 
		bool preemptible_gc_enabled, 
		double gc_hard_threshold,
		unsigned int channel_count, 
		unsigned int chip_no_per_channel, 
		unsigned int die_no_per_chip, 
		unsigned int plane_no_per_die,
		unsigned int block_no_per_plane, 
		unsigned int page_no_per_block,
 		unsigned int sectors_no_per_page, 
		unsigned int sectors_per_mu,
		unsigned int mu_per_page,
		bool use_copyback, 
		double rho, 
		unsigned int max_ongoing_gc_reqs_per_plane, 
		bool dynamic_wearleveling_enabled, 
		bool static_wearleveling_enabled, 
		unsigned int static_wearleveling_threshold, 
		int seed) :
			Sim_Object(id), 	
			address_mapping_unit(address_mapping_unit), 
			block_manager(block_manager), 
			tsu(tsu), 
			flash_controller(flash_controller), 
			force_gc(false),
			block_selection_policy(block_selection_policy), 
			gc_threshold(gc_threshold),	
			use_copyback(use_copyback), 
			preemptible_gc_enabled(preemptible_gc_enabled), 
			gc_hard_threshold(gc_hard_threshold),
			random_generator(seed), 
			max_ongoing_gc_reqs_per_plane(max_ongoing_gc_reqs_per_plane),
			channel_count(channel_count), 
			chip_no_per_channel(chip_no_per_channel), 
			die_no_per_chip(die_no_per_chip), 
			plane_no_per_die(plane_no_per_die),
			block_no_per_plane(block_no_per_plane), 
			pages_no_per_block(page_no_per_block), 
			sectors_no_per_page(sectors_no_per_page),
			sectors_per_mu(sectors_per_mu),
			mu_per_page(mu_per_page),
			dynamic_wearleveling_enabled(dynamic_wearleveling_enabled), 
			static_wearleveling_enabled(static_wearleveling_enabled), 
			static_wearleveling_threshold(static_wearleveling_threshold)
	{
		_my_instance = this;
		block_pool_gc_threshold = (unsigned int)(gc_threshold * (double)block_no_per_plane);
		if (block_pool_gc_threshold < 1) {
			block_pool_gc_threshold = 1;
		}
		block_pool_gc_hard_threshold = (unsigned int)(gc_hard_threshold * (double)block_no_per_plane);
		if (block_pool_gc_hard_threshold < 1) {
			block_pool_gc_hard_threshold = 1;
		}
		random_pp_threshold = (unsigned int)(rho * pages_no_per_block);
		if (block_pool_gc_threshold < max_ongoing_gc_reqs_per_plane) {
			block_pool_gc_threshold = max_ongoing_gc_reqs_per_plane;
		}
	}

	void GC_and_WL_Unit_Base::Setup_triggers()
	{
		Sim_Object::Setup_triggers();
		flash_controller->ConnectToTransactionServicedSignal(handle_transaction_serviced_signal_from_PHY);
	}

	void GC_and_WL_Unit_Base::handle_transaction_serviced_signal_from_PHY(NVM_Transaction_Flash* transaction)
	{
		//FGM - FTL Components:
		Flash_Block_Manager_Base* fbm;
		Address_Mapping_Unit_Base* amu;		
		TSU_Base* tsu;
		Data_Cache_Manager_Base* dcm;
		GC_and_WL_Unit_Base* gc_wl;

		fbm = _my_instance->block_manager;
		amu = _my_instance->address_mapping_unit;
		tsu = _my_instance->tsu; 
		dcm = _my_instance->dcm;
		gc_wl = _my_instance;
	
		PlaneBookKeepingType* pbke = &(fbm->plane_manager[transaction->Address.ChannelID][transaction->Address.ChipID][transaction->Address.DieID][transaction->Address.PlaneID]);

		switch (transaction->Source) {
			case Transaction_Source_Type::USERIO:
			case Transaction_Source_Type::MAPPING:
			case Transaction_Source_Type::CACHE:
				switch (transaction->Type)
				{
					case Transaction_Type::READ:
						fbm->Read_transaction_serviced(transaction->Address);
						break;
					case Transaction_Type::WRITE:
						fbm->Program_transaction_serviced(transaction->Address);
						break;
					default:
						PRINT_ERROR("Unexpected situation in the GC_and_WL_Unit_Base function!")
				}
				if (fbm->Block_has_ongoing_gc_wl(transaction->Address)) {
			
					if (fbm->Can_execute_gc_wl(transaction->Address)) {
						NVM::FlashMemory::Physical_Page_Address gc_wl_candidate_address(transaction->Address);
						Block_Pool_Slot_Type* block = &pbke->Blocks[transaction->Address.BlockID];
						Stats::Total_gc_executions++;
						tsu->Prepare_for_transaction_submit();
						NVM_Transaction_Flash_ER* gc_wl_erase_tr = new NVM_Transaction_Flash_ER(
								Transaction_Source_Type::GC_WL, 
								block->Stream_id, 
								gc_wl_candidate_address);
						
						//If there are some valid pages in block, then prepare flash transactions for page movement
						if (block->Current_page_write_index - block->Invalid_page_count > 0) {
							//address_mapping_unit->Lock_physical_block_for_gc(gc_candidate_address);//Lock the block, so no user request can intervene while the GC is progressing
							NVM_Transaction_Flash_RD* gc_wl_read = NULL;
							NVM_Transaction_Flash_WR* gc_wl_write = NULL;
							for (flash_page_ID_type pageID = 0; pageID < block->Current_page_write_index; pageID++) 
							{
								if (fbm->Is_page_valid(block, pageID)) 
								{
									Stats::Total_page_movements_for_gc++;
									gc_wl_candidate_address.PageID = pageID;
									gc_wl_candidate_address.MUID = 0; 

									if (gc_wl->use_copyback) 
									{ 
										gc_wl_write = new NVM_Transaction_Flash_WR(Transaction_Source_Type::GC_WL, block->Stream_id, gc_wl->sectors_per_mu* SECTOR_SIZE_IN_BYTE,
											NO_LPA, gc_wl->address_mapping_unit->Convert_address_to_ppa(gc_wl_candidate_address), NULL, 0, NULL, 0, INVALID_TIME_STAMP);
										gc_wl_write->ExecutionMode = WriteExecutionModeType::COPYBACK;
										tsu->Submit_transaction(gc_wl_write);
									}
									else 
									{
										//FGM - Generate a new GC RD transaction
										gc_wl_read = new NVM_Transaction_Flash_RD(
											Transaction_Source_Type::GC_WL, 
											block->Stream_id, 
											gc_wl->sectors_per_mu * SECTOR_SIZE_IN_BYTE,
											NO_LPA, 
											amu->Convert_address_to_ppa(gc_wl_candidate_address), 
											gc_wl_candidate_address, 
											NULL, 
											0, 
											NULL, 
											0, 
											INVALID_TIME_STAMP);
										//FGM - Update the new GC RD transaction for every valid mapping unit 
										for (unsigned int mu_idx=1; mu_idx<=gc_wl->mu_per_page-1; mu_idx++)
										{
											if (fbm->Is_mu_valid(block, pageID, mu_idx))
											{
												gc_wl_candidate_address.MUID = mu_idx;
												gc_wl_read->update_transaction(
													gc_wl->sectors_per_mu * SECTOR_SIZE_IN_BYTE,
													NO_LPA,
													amu->Convert_address_to_ppa(gc_wl_candidate_address),
													gc_wl->sectors_per_mu,
													0,
													INVALID_TIME_STAMP,
													NULL);
											}
										}
										gc_wl_read->RelatedErase = gc_wl_erase_tr;
										tsu->Submit_transaction(gc_wl_read);
									}
								}
							}
						}
						block->Erase_transaction = gc_wl_erase_tr;
						tsu->Schedule();
					}
				}

				return;
		}

		switch (transaction->Type) {
			case Transaction_Type::READ:
			{
				PPA_type ppa;
				MPPN_type mppa;
				page_status_type page_status_bitmap;

				NVM_Transaction_Flash_RD* read_transaction = (NVM_Transaction_Flash_RD*)transaction;
				NVM_Transaction_Flash_WR* gc_write;

				bool full;

				if (pbke->Blocks[transaction->Address.BlockID].Holds_mapping_data) {
					gc_wl->address_mapping_unit->Get_translation_mapping_info_for_gc(transaction->Stream_id, (MVPN_type)transaction->get_lpa(), mppa, page_status_bitmap);
					//There has been no write on the page since GC start, and it is still valid
					if (mppa == transaction->get_ppa()) {
						tsu->Prepare_for_transaction_submit();
						read_transaction->get_related_write()->write_sectors_bitmap = FULL_PROGRAMMED_PAGE;
						read_transaction->get_related_write()->set_lpa( transaction->get_lpa() );
						read_transaction->get_related_write()->set_related_read( NULL);
						gc_wl->address_mapping_unit->Allocate_new_page_for_gc(read_transaction->get_related_write(), pbke->Blocks[transaction->Address.BlockID].Holds_mapping_data);
						tsu->Submit_transaction(read_transaction->get_related_write() );
						tsu->Schedule();
					} else {
						PRINT_ERROR("Inconsistency found when moving a page for GC/WL!")
					}
				} 
				else 
				{
					//FGM - Must check that the incoming LPAs are locked in AMU
					for (unsigned int mu_idx=0; mu_idx<transaction->num_lpas(); mu_idx++)
					{	
						for (unsigned int i = 0; i< gc_wl->mu_per_page; i++)
						{						
							//FGM - Search for the right lpa 
							amu->Get_data_mapping_info_for_gc(transaction->Stream_id, transaction->get_waiting_lpas(i), ppa, page_status_bitmap);
							if (ppa == transaction->get_ppa(mu_idx) ) 
							{
								transaction->replace_lpa(transaction->get_waiting_lpas(i), mu_idx , i);
								break;
							}
	
						} 
						//FGM - This function inserts the transaction in GC buffer and also returns if the buffer is full or not.
						full = dcm->Insert_to_GC_buffer ( transaction->Stream_id, transaction->get_lpa(mu_idx), page_status_bitmap);
						if (full)
						{ 
							tsu->Prepare_for_transaction_submit();
							gc_write =gc_wl-> dcm->Merge_and_flush_GC_buffer(transaction->Stream_id);
							gc_write->ExecutionMode=WriteExecutionModeType::SIMPLE; 
							gc_write->RelatedErase = read_transaction->RelatedErase;
							
							amu->Allocate_new_page_for_gc( gc_write, pbke->Blocks[transaction->Address.BlockID].Holds_mapping_data);

							tsu->Submit_transaction(gc_write);
							tsu->Schedule();	
						}		
					} 
				}
				break;
			}
			case Transaction_Type::WRITE:
				//FGM - Iterate through the mapping units of the page before removing the barrier 
				for (unsigned int mu_idx = 0; mu_idx< transaction->num_lpas(); mu_idx++)
				{
					amu->Remove_barrier_for_accessing_lpa(transaction->Stream_id, transaction->get_lpa(mu_idx));
					DEBUG(Simulator->Time() << ": LPA=" << (MVPN_type)transaction->get_lpa(mu_idx) << " unlocked!!");
				}
				break;

			case Transaction_Type::ERASE:
				pbke->Ongoing_erase_operations.erase(pbke->Ongoing_erase_operations.find(transaction->Address.BlockID));
				fbm->Add_erased_block_to_pool(transaction->Address);
				fbm->GC_WL_finished(transaction->Address);
				if (gc_wl->check_static_wl_required(transaction->Address)) {
					gc_wl->run_static_wearleveling(transaction->Address);
				}
				amu->Start_servicing_writes_for_overfull_plane(transaction->Address);//Must be inovked after above statements since it may lead to flash page consumption for waiting program transactions

				if (gc_wl->Stop_servicing_writes(transaction->Address)) {
					gc_wl->Check_gc_required(pbke->Get_free_block_pool_size(), transaction->Address);
				}
				break;
			} 
	}

	void GC_and_WL_Unit_Base::Start_simulation()
	{
	}

	void GC_and_WL_Unit_Base::Validate_simulation_config()
	{
	}

	void GC_and_WL_Unit_Base::Execute_simulator_event(MQSimEngine::Sim_Event* ev)
	{
	}

	GC_Block_Selection_Policy_Type GC_and_WL_Unit_Base::Get_gc_policy()
	{
		return block_selection_policy;
	}

	unsigned int GC_and_WL_Unit_Base::Get_GC_policy_specific_parameter()
	{
		switch (block_selection_policy) {
			case GC_Block_Selection_Policy_Type::RGA:
				return rga_set_size;
			case GC_Block_Selection_Policy_Type::RANDOM_PP:
				return random_pp_threshold;
		}

		return 0;
	}

	unsigned int GC_and_WL_Unit_Base::Get_minimum_number_of_free_pages_before_GC()
	{
		return block_pool_gc_threshold;
		/*if (preemptible_gc_enabled)
			return block_pool_gc_hard_threshold;
		else return block_pool_gc_threshold;*/
	}

	bool GC_and_WL_Unit_Base::Use_dynamic_wearleveling()
	{
		return dynamic_wearleveling_enabled;
	}

	inline bool GC_and_WL_Unit_Base::Use_static_wearleveling()
	{
		return static_wearleveling_enabled;
	}
	
	bool GC_and_WL_Unit_Base::Stop_servicing_writes(const NVM::FlashMemory::Physical_Page_Address& plane_address)
	{
		PlaneBookKeepingType* pbke = &(_my_instance->block_manager->plane_manager[plane_address.ChannelID][plane_address.ChipID][plane_address.DieID][plane_address.PlaneID]);
		return block_manager->Get_pool_size(plane_address) < max_ongoing_gc_reqs_per_plane;
	}

	bool GC_and_WL_Unit_Base::is_safe_gc_wl_candidate(const PlaneBookKeepingType* plane_record, const flash_block_ID_type gc_wl_candidate_block_id)
	{
		//The block shouldn't be a current write frontier
		for (unsigned int stream_id = 0; stream_id < address_mapping_unit->Get_no_of_input_streams(); stream_id++) {
			if ((&plane_record->Blocks[gc_wl_candidate_block_id]) == plane_record->Data_wf[stream_id]
				|| (&plane_record->Blocks[gc_wl_candidate_block_id]) == plane_record->Translation_wf[stream_id]
				|| (&plane_record->Blocks[gc_wl_candidate_block_id]) == plane_record->GC_wf[stream_id]) {
				return false;
			}
		}

		//The block shouldn't have an ongoing program request (all pages must already be written)
		if (plane_record->Blocks[gc_wl_candidate_block_id].Ongoing_user_program_count > 0) {
			return false;
		}

		if (plane_record->Blocks[gc_wl_candidate_block_id].Has_ongoing_gc_wl) {
			return false;
		}

		return true;
	}

	inline bool GC_and_WL_Unit_Base::check_static_wl_required(const NVM::FlashMemory::Physical_Page_Address plane_address)
	{
		return static_wearleveling_enabled && (block_manager->Get_min_max_erase_difference(plane_address) >= static_wearleveling_threshold);
	}

	void GC_and_WL_Unit_Base::run_static_wearleveling(const NVM::FlashMemory::Physical_Page_Address plane_address)
	{

		PlaneBookKeepingType* pbke = block_manager->Get_plane_bookkeeping_entry(plane_address);
		flash_block_ID_type wl_candidate_block_id = block_manager->Get_coldest_block_id(plane_address);
		if (!is_safe_gc_wl_candidate(pbke, wl_candidate_block_id)) {
			return;
		}

		NVM::FlashMemory::Physical_Page_Address wl_candidate_address(plane_address);
		wl_candidate_address.BlockID = wl_candidate_block_id;
		Block_Pool_Slot_Type* block = &pbke->Blocks[wl_candidate_block_id];

		//Run the state machine to protect against race condition
		block_manager->GC_WL_started(wl_candidate_block_id);
		pbke->Ongoing_erase_operations.insert(wl_candidate_block_id);
		address_mapping_unit->Set_barrier_for_accessing_physical_block(wl_candidate_address);//Lock the block, so no user request can intervene while the GC is progressing
		if (block_manager->Can_execute_gc_wl(wl_candidate_address)) 
		{ 
			//If there are ongoing requests targeting the candidate block, the gc execution should be postponed
			Stats::Total_wl_executions++;
			tsu->Prepare_for_transaction_submit();

			NVM_Transaction_Flash_ER* wl_erase_tr = new NVM_Transaction_Flash_ER(
						Transaction_Source_Type::GC_WL,
						pbke->Blocks[wl_candidate_block_id].Stream_id, 
						wl_candidate_address);

			if (block->Current_page_write_index - block->Invalid_page_count > 0) 
			{ 	
				//If there are some valid pages in block, then prepare flash transactions for page movement
				NVM_Transaction_Flash_RD* wl_read = NULL;
				NVM_Transaction_Flash_WR* wl_write = NULL;
				for (flash_page_ID_type pageID = 0; pageID < block->Current_page_write_index; pageID++) 
				{
					if (block_manager->Is_page_valid(block, pageID)) 
					{
						Stats::Total_page_movements_for_gc;
						wl_candidate_address.PageID = pageID;
						wl_candidate_address.MUID = 0;

						if (use_copyback) 
						{ 
							wl_write = new NVM_Transaction_Flash_WR(Transaction_Source_Type::GC_WL, block->Stream_id, sectors_per_mu * SECTOR_SIZE_IN_BYTE,
								NO_LPA, address_mapping_unit->Convert_address_to_ppa(wl_candidate_address), NULL, 0, NULL, 0, INVALID_TIME_STAMP);
							wl_write->ExecutionMode = WriteExecutionModeType::COPYBACK;
							tsu->Submit_transaction(wl_write);
						} 
						else 
						{
							//FG - 
							bool flag = false;
							for (unsigned int mu_idx = 0; mu_idx < mu_per_page; mu_idx ++)
							{
								if (block_manager->Is_mu_valid(block, pageID, mu_idx))
								{ 
									
									if (!flag)
									{
									
										wl_candidate_address.MUID = mu_idx;	
										
										wl_read = new NVM_Transaction_Flash_RD(
											Transaction_Source_Type::GC_WL, 
											block->Stream_id, 
											sectors_per_mu* SECTOR_SIZE_IN_BYTE,
											NO_LPA, 
											address_mapping_unit->Convert_address_to_ppa(wl_candidate_address),
											wl_candidate_address, 
											NULL, 
											0, 
											NULL, 
											0, 
											INVALID_TIME_STAMP);

											flag = true;
									}
									else
									{
										
										wl_candidate_address.MUID = mu_idx;	
										
										wl_read->update_transaction(
											sectors_per_mu * SECTOR_SIZE_IN_BYTE,
											NO_LPA,
											address_mapping_unit->Convert_address_to_ppa(wl_candidate_address),
											sectors_per_mu,
											0,
											INVALID_TIME_STAMP,
											NULL);
									}
									
								}
							}//for
							wl_read->RelatedErase = wl_erase_tr;
							tsu->Submit_transaction(wl_read);		
						}
					}//is page valid
				}//for 	
			}//if 
			block->Erase_transaction = wl_erase_tr;
			tsu->Submit_transaction(wl_erase_tr);

			tsu->Schedule();
		}
	}
}
