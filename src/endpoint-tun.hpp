#ifndef ENDPOINT_TUN_H
#define ENDPOINT_TUN_H

#include <boost/asio.hpp>

class tun_service : public boost::asio::detail::service_base<tun_service> {
	private:
		typedef boost::asio::detail::reactive_descriptor_service service_impl_type;
	public:
		typedef service_impl_type::implementation_type implementation_type;
		typedef service_impl_type::native_handle_type native_type;
		typedef service_impl_type::native_handle_type native_handle_type;

		explicit tun_service(boost::asio::io_service& io_service) : boost::asio::detail::service_base<tun_service>(io_service), service_impl_(io_service) {}

		void construct(implementation_type& impl)
		{
			service_impl_.construct(impl);
		}

		void destroy(implementation_type& impl)
		{
			service_impl_.destroy(impl);
		}

		boost::system::error_code assign(implementation_type& impl,
			const native_handle_type& native_descriptor,
			boost::system::error_code& ec)
		{
			return service_impl_.assign(impl, native_descriptor, ec);
		}

		bool is_open(const implementation_type& impl) const
		{
			return service_impl_.is_open(impl);
		}

		boost::system::error_code close(implementation_type& impl,
			boost::system::error_code& ec)
		{
			return service_impl_.close(impl, ec);
		}

		native_handle_type native_handle(implementation_type& impl)
		{
			return service_impl_.native_handle(impl);
		}

		native_handle_type release(implementation_type& impl)
		{
			return service_impl_.release(impl);
		}

		boost::system::error_code cancel(implementation_type& impl,
			boost::system::error_code& ec)
		{
			return service_impl_.cancel(impl, ec);
		}

		template <typename IoControlCommand>
		boost::system::error_code io_control(implementation_type& impl,
			IoControlCommand& command, boost::system::error_code& ec)
		{
			return service_impl_.io_control(impl, command, ec);
		}

		bool non_blocking(const implementation_type& impl) const
		{
			return service_impl_.non_blocking(impl);
		}

		boost::system::error_code non_blocking(implementation_type& impl,
			bool mode, boost::system::error_code& ec)
		{
			return service_impl_.non_blocking(impl, mode, ec);
		}

		bool native_non_blocking(const implementation_type& impl) const
		{
			return service_impl_.native_non_blocking(impl);
		}

		boost::system::error_code native_non_blocking(implementation_type& impl,
			bool mode, boost::system::error_code& ec)
		{
			return service_impl_.native_non_blocking(impl, mode, ec);
		}

		template <typename ConstBufferSequence>
		std::size_t write_some(implementation_type& impl,
			const ConstBufferSequence& buffers, boost::system::error_code& ec)
		{
			return service_impl_.write_some(impl, buffers, ec);
		}

		template <typename ConstBufferSequence, typename WriteHandler>
		BOOST_ASIO_INITFN_RESULT_TYPE(WriteHandler,
			void (boost::system::error_code, std::size_t))
		async_write_some(implementation_type& impl,
			const ConstBufferSequence& buffers,
			BOOST_ASIO_MOVE_ARG(WriteHandler) handler)
		{
			boost::asio::detail::async_result_init<
			WriteHandler, void (boost::system::error_code, std::size_t)> init(
				BOOST_ASIO_MOVE_CAST(WriteHandler)(handler));

			service_impl_.async_write_some(impl, buffers, init.handler);

			return init.result.get();
		}

		template <typename MutableBufferSequence>
		std::size_t read_some(implementation_type& impl,
			const MutableBufferSequence& buffers, boost::system::error_code& ec)
		{
			return service_impl_.read_some(impl, buffers, ec);
		}

		template <typename MutableBufferSequence, typename ReadHandler>
		BOOST_ASIO_INITFN_RESULT_TYPE(ReadHandler,
			void (boost::system::error_code, std::size_t))
		async_read_some(implementation_type& impl,
			const MutableBufferSequence& buffers,
			BOOST_ASIO_MOVE_ARG(ReadHandler) handler)
		{
			boost::asio::detail::async_result_init<
			ReadHandler, void (boost::system::error_code, std::size_t)> init(
				BOOST_ASIO_MOVE_CAST(ReadHandler)(handler));

			service_impl_.async_read_some(impl, buffers, init.handler);

			return init.result.get();
		}

	private:
		void shutdown_service() {
			service_impl_.shutdown_service();
		}

		service_impl_type service_impl_;
};

class tun : public boost::asio::posix::basic_descriptor<tun_service> {
	public:
		tun(boost::asio::io_service &io_service, const std::string &&name);

		template <typename MutableBufferSequence, typename ReadHandler>
		BOOST_ASIO_INITFN_RESULT_TYPE(ReadHandler,
			void (boost::system::error_code, std::size_t))
		async_receive(const MutableBufferSequence& buffers,
			BOOST_ASIO_MOVE_ARG(ReadHandler) handler)
		{
			return get_service().async_read_some(get_implementation(), buffers, BOOST_ASIO_MOVE_CAST(ReadHandler)(handler));
		}

		template <typename ConstBufferSequence, typename WriteHandler>
		BOOST_ASIO_INITFN_RESULT_TYPE(WriteHandler,
			void (boost::system::error_code, std::size_t))
		async_send(const ConstBufferSequence& buffers,
			BOOST_ASIO_MOVE_ARG(WriteHandler) handler)
		{
			return get_service().async_write_some(get_implementation(), buffers, BOOST_ASIO_MOVE_CAST(WriteHandler)(handler));
		}
};

#endif /* end of include guard: ENDPOINT_TUN_H */
